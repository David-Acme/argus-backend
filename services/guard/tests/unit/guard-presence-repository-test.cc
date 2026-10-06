#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <shared/repositories/presence/presence-repository.hxx>

#include "guard-fakes.hxx"

using guard_test::GuardBoot;
using guard_test::scalar;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-presence-repository-test");
  return shared;
}

int64_t homeId()
{
  return std::stoll(
      scalar("SELECT id FROM guard_environment WHERE is_default = 1"));
}

int64_t addEnvironment(const std::string& name)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, lan_presence) VALUES (?, "
      "'office', 0)",
      name);
  return std::stoll(scalar("SELECT id FROM guard_environment WHERE name = '" +
                           name + "'"));
}

PresenceRow homeRow(int64_t userId, int64_t environmentId, int64_t at)
{
  return {.userId = userId,
          .environmentId = environmentId,
          .state = PresenceState::Home,
          .source = PresenceSource::LanSession,
          .since = at,
          .lastHomeAt = at,
          .lastSignalAt = at};
}

}

TEST_CASE("an upsert writes one row per user and environment")
{
  (void)boot();
  const PresenceRepository repository;
  const int64_t home = homeId();
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(11, home, 100))));
  PresenceRow later = homeRow(11, home, 100);
  later.lastHomeAt = 160;
  later.lastSignalAt = 160;
  REQUIRE(drogon::sync_wait(repository.upsert(later)));
  const auto found =
      drogon::sync_wait(repository.find({.userId = 11, .environmentId = home}));
  REQUIRE(found.has_value());
  const PresenceRow row = found.value_or(PresenceRow{});
  CHECK(row.since == 100);
  CHECK(row.lastHomeAt == 160);
  CHECK(row.state == PresenceState::Home);
  CHECK(row.source == PresenceSource::LanSession);
  CHECK(scalar("SELECT COUNT(*) FROM guard_presence WHERE user_id = 11") ==
        "1");
  drogon::sync_wait(repository.removeUser(11));
}

TEST_CASE("an upsert for an environment that does not exist writes nothing")
{
  (void)boot();
  const PresenceRepository repository;
  CHECK_FALSE(drogon::sync_wait(repository.upsert(homeRow(12, 99999, 100))));
  CHECK(scalar("SELECT COUNT(*) FROM guard_presence WHERE user_id = 12") ==
        "0");
}

TEST_CASE("the default environment is the one that reads the home network")
{
  (void)boot();
  DbService::client()->execSqlSync(
      "UPDATE guard_environment SET lan_presence = 1 WHERE is_default = 1");
  const PresenceRepository repository;
  const int64_t office = addEnvironment("Oficina presencia");
  const auto lan = drogon::sync_wait(repository.lanEnvironments());
  REQUIRE(lan.size() == 1);
  CHECK(lan.front() == homeId());
  CHECK(lan.front() != office);
}

TEST_CASE("a lookup filters by environment and by the users asked for")
{
  (void)boot();
  const PresenceRepository repository;
  const int64_t home = homeId();
  const int64_t office = addEnvironment("Oficina lookup");
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(21, home, 100))));
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(22, home, 100))));
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(21, office, 100))));
  const auto everyone = drogon::sync_wait(
      repository.forEnvironment({.environmentId = home, .userIds = {}}));
  CHECK(everyone.size() == 2);
  const auto one = drogon::sync_wait(
      repository.forEnvironment({.environmentId = home, .userIds = {22}}));
  REQUIRE(one.size() == 1);
  CHECK(one.front().userId == 22);
  CHECK(presence::stateOf(everyone, 22) == PresenceState::Home);
  CHECK(presence::stateOf(everyone, 23) == PresenceState::Unknown);
  const auto removed = drogon::sync_wait(repository.removeUser(21));
  CHECK(removed.size() == 2);
  CHECK(drogon::sync_wait(repository.forUser(21)).empty());
  drogon::sync_wait(repository.removeUser(22));
}

TEST_CASE("home rows past the timeout become away, and stale rows go")
{
  (void)boot();
  const PresenceRepository repository;
  const int64_t home = homeId();
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(31, home, 1000))));
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(32, home, 5000))));
  const auto expired = drogon::sync_wait(
      repository.expireHome({.homeBefore = 2000, .at = 6000}));
  REQUIRE(expired.size() == 1);
  CHECK(expired.front().userId == 31);
  CHECK(expired.front().state == PresenceState::Away);
  CHECK(expired.front().source == PresenceSource::Timeout);
  CHECK(expired.front().since == 6000);
  CHECK(drogon::sync_wait(repository.purgeStale(5500)) == 1);
  CHECK(drogon::sync_wait(repository.userIds()) == std::vector<int64_t>{31});
  drogon::sync_wait(repository.removeUser(31));
}

TEST_CASE("removing an environment removes its presence rows")
{
  (void)boot();
  DbService::client()->execSqlSync("PRAGMA foreign_keys = ON");
  const PresenceRepository repository;
  const int64_t office = addEnvironment("Oficina borrada");
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(41, office, 100))));
  DbService::client()->execSqlSync("DELETE FROM guard_environment WHERE id = ?",
                                   office);
  CHECK(drogon::sync_wait(repository.forUser(41)).empty());
}

TEST_CASE("overall presence folds the environments of one user")
{
  const std::vector<PresenceRow> none;
  CHECK(presence::overall(none) == PresenceState::Unknown);
  std::vector<PresenceRow> rows{homeRow(1, 1, 1), homeRow(1, 2, 1)};
  rows[0].state = PresenceState::Away;
  CHECK(presence::overall(rows) == PresenceState::Home);
  rows[1].state = PresenceState::Away;
  CHECK(presence::overall(rows) == PresenceState::Away);
  CHECK(presenceStateToString(PresenceState::Unknown) == "unknown");
  CHECK(presenceSourceFromString("tunnel_session") ==
        PresenceSource::TunnelSession);
  CHECK(presenceSourceToString(PresenceSource::AppActivity) == "app_activity");
}

TEST_CASE("a long stay at home is kept while its signals keep coming")
{
  (void)boot();
  const PresenceRepository repository;
  const int64_t home = homeId();
  PresenceRow settled = homeRow(51, home, 100);
  settled.lastHomeAt = 9000;
  settled.lastSignalAt = 9000;
  REQUIRE(drogon::sync_wait(repository.upsert(settled)));
  CHECK(drogon::sync_wait(repository.purgeStale(5000)) == 0);
  CHECK(drogon::sync_wait(repository.find({.userId = 51, .environmentId = home})));
  drogon::sync_wait(repository.removeUser(51));
}

TEST_CASE("a transition reads and writes the row in one transaction")
{
  (void)boot();
  const PresenceRepository repository;
  const int64_t home = homeId();
  REQUIRE(drogon::sync_wait(repository.upsert(homeRow(61, home, 100))));
  const PresenceDecision decision = drogon::sync_wait(repository.transition(
      {.key = {.userId = 61, .environmentId = home},
       .decide = [](const std::optional<PresenceRow>& current) {
         PresenceRow row = current.value_or(PresenceRow{});
         row.lastSignalAt = 200;
         return PresenceDecision{.row = row, .write = current.has_value(), .changed = false};
       }}));
  CHECK(decision.write);
  const auto touched =
      drogon::sync_wait(repository.find({.userId = 61, .environmentId = home}));
  REQUIRE(touched.has_value());
  CHECK(touched.value_or(PresenceRow{}).lastSignalAt == 200);
  const PresenceDecision skipped = drogon::sync_wait(repository.transition(
      {.key = {.userId = 62, .environmentId = home},
       .decide = [](const std::optional<PresenceRow>& current) {
         return PresenceDecision{.row = {}, .write = current.has_value(), .changed = false};
       }}));
  CHECK_FALSE(skipped.write);
  drogon::sync_wait(repository.removeUser(61));
}
