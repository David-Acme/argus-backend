#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <app/rpc/presence-rpc-service.hxx>
#include <feature/presence/dtos/response-presence-dto.hxx>
#include <feature/presence/services/presence-engine.hxx>
#include <feature/presence/services/presence-service.hxx>
#include <guard/guard-presence-client.hxx>
#include <sync/sync-change.hxx>

#include <map>
#include <mutex>
#include <set>

#include "guard-fakes.hxx"

using guard_test::GuardBoot;
using guard_test::scalar;

namespace
{

constexpr int64_t kStart = 1800000000;

GuardBoot& boot()
{
  static GuardBoot shared("guard-presence-test");
  return shared;
}

class FakeDirectory final : public PresenceDirectory
{
public:
  std::optional<bool> presenceConsent(int64_t userId) const override
  {
    const std::scoped_lock lock(mutex_);
    ++consentQuestions;
    if (!reachable)
      return std::nullopt;
    return consenting.contains(userId);
  }

  std::optional<int64_t> userOfPerson(int64_t personId) const override
  {
    const std::scoped_lock lock(mutex_);
    const auto found = persons.find(personId);
    if (found == persons.end())
      return std::nullopt;
    return found->second;
  }

  std::optional<std::vector<int64_t>> consentingUsers() const override
  {
    const std::scoped_lock lock(mutex_);
    if (!reachable)
      return std::nullopt;
    return std::vector<int64_t>(consenting.begin(), consenting.end());
  }

  std::set<int64_t> consenting;
  std::map<int64_t, int64_t> persons;
  bool reachable{true};
  mutable int consentQuestions{0};

private:
  mutable std::mutex mutex_;
};

class RecordingPublisher final : public PresencePublisher
{
public:
  void publish(const PresenceChange& change) override
  {
    changes.push_back(change);
  }

  std::vector<PresenceChange> changes;
};

int64_t homeId()
{
  return std::stoll(
      scalar("SELECT id FROM guard_environment WHERE is_default = 1"));
}

int64_t environmentNamed(const std::string& name, bool lan)
{
  const std::string existing =
      scalar("SELECT id FROM guard_environment WHERE name = '" + name + "'");
  if (!existing.empty())
    return std::stoll(existing);
  DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, lan_presence) VALUES (?, "
      "'office', ?)",
      name, lan ? 1 : 0);
  return std::stoll(
      scalar("SELECT id FROM guard_environment WHERE name = '" + name + "'"));
}

void prepare()
{
  (void)boot();
  DbService::client()->execSqlSync(
      "UPDATE guard_environment SET lan_presence = 1 WHERE is_default = 1");
  DbService::client()->execSqlSync("DELETE FROM guard_presence");
}

GuardPresenceConfig testConfig()
{
  return {.enabled = true,
          .awayTimeoutSeconds = 2700,
          .tunnelGraceSeconds = 90,
          .retentionSeconds = 2592000,
          .sweepSeconds = 60,
          .consentRefreshSeconds = 600};
}

struct Harness
{
  FakeDirectory directory;
  RecordingPublisher publisher;
  std::vector<int64_t> disabled;
  PresenceService service{{.bus = nullptr,
                           .directory = &directory,
                           .publisher = &publisher,
                           .onAccountDisabled = [this](int64_t userId) -> drogon::Task<void> {
                             disabled.push_back(userId);
                             co_return;
                           }},
                          testConfig()};
};

struct SessionSignalSpec
{
  int64_t userId{0};
  std::string origin;
  std::string platform;
  int64_t at{0};
};

Json::Value sessionEvent(const SessionSignalSpec& spec)
{
  Json::Value event(Json::objectValue);
  event["userId"] = static_cast<Json::Int64>(spec.userId);
  event["sessionId"] = "0123456789abcdef0123456789abcdef";
  event["origin"] = spec.origin;
  event["platform"] = spec.platform;
  event["at"] = static_cast<Json::Int64>(spec.at);
  return event;
}

struct KnownSeenSpec
{
  int64_t personId{0};
  int64_t environmentId{0};
  bool passerby{false};
  int64_t at{0};
};

Json::Value knownSeenEvent(const KnownSeenSpec& spec)
{
  Json::Value event(Json::objectValue);
  event["eventId"] = "evt-" + std::to_string(spec.at);
  event["personId"] = static_cast<Json::Int64>(spec.personId);
  event["environmentId"] = static_cast<Json::Int64>(spec.environmentId);
  event["passerby"] = spec.passerby;
  event["at"] = static_cast<Json::Int64>(spec.at);
  return event;
}

Json::Value userChange(int64_t userId, const Json::Value& row)
{
  Json::Value event(Json::objectValue);
  event[sync_change::kKindField] = sync_change::kKindIdentity;
  event[sync_change::kTableField] = "user";
  event[sync_change::kRecordIdField] = static_cast<Json::Int64>(userId);
  event[sync_change::kRowField] = row;
  return event;
}

std::optional<PresenceRow> rowOf(int64_t userId, int64_t environmentId)
{
  return drogon::sync_wait(PresenceRepository{}.find(
      {.userId = userId, .environmentId = environmentId}));
}

PresenceRow homeAt(int64_t at, PresenceSource source)
{
  return {.userId = 5,
          .environmentId = 1,
          .state = PresenceState::Home,
          .source = source,
          .since = at,
          .lastHomeAt = at,
          .lastSignalAt = at};
}

PresenceOutcome applyTo(const std::optional<PresenceRow>& current,
                        PresenceSignalKind kind, int64_t at)
{
  const PresenceRules rules;
  const PresenceSignal signal{
      .userId = 5, .environmentId = 1, .kind = kind, .at = at};
  return presence_engine::apply(
      {.current = current, .signal = signal, .rules = rules});
}

}

TEST_CASE("a session signal maps to a presence signal by origin and platform")
{
  using presence_engine::kindOf;
  CHECK(kindOf({.origin = SessionOrigin::Lan,
                .platform = SessionPlatform::Android}) ==
        PresenceSignalKind::LanSession);
  CHECK(kindOf({.origin = SessionOrigin::Lan,
                .platform = SessionPlatform::Ios}) ==
        PresenceSignalKind::LanSession);
  CHECK(kindOf({.origin = SessionOrigin::Lan,
                .platform = SessionPlatform::Desktop}) ==
        PresenceSignalKind::AppActivity);
  CHECK(kindOf({.origin = SessionOrigin::Lan,
                .platform = SessionPlatform::Unknown}) ==
        PresenceSignalKind::AppActivity);
  CHECK(kindOf({.origin = SessionOrigin::Tunnel,
                .platform = SessionPlatform::Web}) ==
        PresenceSignalKind::TunnelSession);
  CHECK_FALSE(kindOf({.origin = SessionOrigin::Loopback,
                      .platform = SessionPlatform::Android}));
  CHECK_FALSE(kindOf({.origin = SessionOrigin::External,
                      .platform = SessionPlatform::Android}));
}

TEST_CASE("a home signal makes a person home and keeps the first arrival")
{
  const auto first = applyTo(std::nullopt, PresenceSignalKind::LanSession, 100);
  CHECK(first.write);
  CHECK(first.changed);
  CHECK(first.row.state == PresenceState::Home);
  CHECK(first.row.source == PresenceSource::LanSession);
  CHECK(first.row.since == 100);

  const auto again = applyTo(first.row, PresenceSignalKind::AppActivity, 160);
  CHECK(again.write);
  CHECK_FALSE(again.changed);
  CHECK(again.row.since == 100);
  CHECK(again.row.lastHomeAt == 160);
  CHECK(again.row.source == PresenceSource::AppActivity);

  const auto seen = applyTo(std::nullopt, PresenceSignalKind::Camera, 50);
  CHECK(seen.row.state == PresenceState::Home);
  CHECK(seen.row.source == PresenceSource::Camera);
}

TEST_CASE("the tunnel means away, after a grace that absorbs a phone "
          "leaving the wifi")
{
  const PresenceRow home = homeAt(1000, PresenceSource::LanSession);
  const auto racing = applyTo(home, PresenceSignalKind::TunnelSession, 1030);
  CHECK_FALSE(racing.write);
  CHECK_FALSE(racing.changed);

  const auto left = applyTo(home, PresenceSignalKind::TunnelSession, 1100);
  CHECK(left.write);
  CHECK(left.changed);
  CHECK(left.row.state == PresenceState::Away);
  CHECK(left.row.source == PresenceSource::TunnelSession);
  CHECK(left.row.since == 1100);
  CHECK(left.row.lastHomeAt == 1000);

  const auto still = applyTo(left.row, PresenceSignalKind::TunnelSession, 1200);
  CHECK(still.write);
  CHECK_FALSE(still.changed);
  CHECK(still.row.since == 1100);

  const auto back = applyTo(left.row, PresenceSignalKind::LanSession, 1300);
  CHECK(back.changed);
  CHECK(back.row.state == PresenceState::Home);
  CHECK(back.row.since == 1300);
}

TEST_CASE("a timed-out away is confirmed by the tunnel at once, and stale "
          "signals change nothing")
{
  PresenceRow timedOut = homeAt(1000, PresenceSource::Timeout);
  timedOut.state = PresenceState::Away;
  timedOut.lastSignalAt = 4000;
  const auto confirmed =
      applyTo(timedOut, PresenceSignalKind::TunnelSession, 4010);
  CHECK(confirmed.changed);
  CHECK(confirmed.row.source == PresenceSource::TunnelSession);
  CHECK(confirmed.row.since == timedOut.since);

  const auto stale = applyTo(timedOut, PresenceSignalKind::LanSession, 3999);
  CHECK_FALSE(stale.write);

  const auto broken = applyTo(std::nullopt, PresenceSignalKind::LanSession, 0);
  CHECK_FALSE(broken.write);
}

TEST_CASE("without consent nothing is stored, and an unreachable identity "
          "stores nothing either")
{
  prepare();
  Harness harness;
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 70, .origin = "lan", .platform = "android", .at = kStart})));
  CHECK_FALSE(rowOf(70, homeId()).has_value());
  CHECK(harness.publisher.changes.empty());

  harness.directory.reachable = false;
  harness.directory.consenting.insert(71);
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 71, .origin = "lan", .platform = "android", .at = kStart})));
  CHECK_FALSE(rowOf(71, homeId()).has_value());
}

TEST_CASE("a phone on the home network is home in every environment that "
          "reads it, and the tunnel turns it away")
{
  prepare();
  const int64_t office = environmentNamed("Oficina sin red", false);
  const int64_t annex = environmentNamed("Anexo con red", true);
  Harness harness;
  harness.directory.consenting.insert(72);

  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 72, .origin = "lan", .platform = "ios", .at = kStart})));
  const auto home = rowOf(72, homeId());
  REQUIRE(home.has_value());
  CHECK(home.value_or(PresenceRow{}).state == PresenceState::Home);
  CHECK(home.value_or(PresenceRow{}).source == PresenceSource::LanSession);
  CHECK(rowOf(72, annex).has_value());
  CHECK_FALSE(rowOf(72, office).has_value());
  REQUIRE(harness.publisher.changes.size() == 2);
  CHECK(harness.publisher.changes.back().overall == PresenceState::Home);

  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 72, .origin = "loopback", .platform = "ios", .at = kStart + 500})));
  CHECK(harness.publisher.changes.size() == 2);

  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 72, .origin = "tunnel", .platform = "ios", .at = kStart + 600})));
  CHECK(rowOf(72, homeId()).value_or(PresenceRow{}).state ==
        PresenceState::Away);
  REQUIRE(harness.publisher.changes.size() == 4);
  CHECK(harness.publisher.changes.at(2).overall == PresenceState::Home);
  CHECK(harness.publisher.changes.back().overall == PresenceState::Away);
  CHECK(harness.publisher.changes.back().row.source ==
        PresenceSource::TunnelSession);
  CHECK(harness.directory.consentQuestions == 1);
  DbService::client()->execSqlSync("DELETE FROM guard_environment WHERE id = ?",
                                   annex);
}

TEST_CASE("a recognized face makes its user home in that camera's "
          "environment, never a passer-by or a face without an account")
{
  prepare();
  const int64_t office = environmentNamed("Oficina camara", false);
  Harness harness;
  harness.directory.consenting.insert(73);
  harness.directory.persons[900] = 73;

  drogon::sync_wait(
      harness.service.onKnownSeen(knownSeenEvent({.personId = 900, .environmentId = office, .passerby = true, .at = kStart})));
  CHECK_FALSE(rowOf(73, office).has_value());

  drogon::sync_wait(
      harness.service.onKnownSeen(knownSeenEvent({.personId = 901, .environmentId = office, .passerby = false, .at = kStart})));
  CHECK(harness.publisher.changes.empty());

  drogon::sync_wait(
      harness.service.onKnownSeen(knownSeenEvent({.personId = 900, .environmentId = office, .passerby = false, .at = kStart})));
  const auto seen = rowOf(73, office);
  REQUIRE(seen.has_value());
  CHECK(seen.value_or(PresenceRow{}).source == PresenceSource::Camera);
  CHECK_FALSE(rowOf(73, homeId()).has_value());
}

TEST_CASE("home without a fresh signal times out to away, and old rows go")
{
  prepare();
  Harness harness;
  harness.directory.consenting = {74, 75};
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 74, .origin = "lan", .platform = "android", .at = kStart})));
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 75, .origin = "lan", .platform = "android", .at = kStart + 2000})));
  harness.publisher.changes.clear();

  drogon::sync_wait(harness.service.sweep(kStart + 2701));
  const auto expired = rowOf(74, homeId());
  CHECK(expired.value_or(PresenceRow{}).state == PresenceState::Away);
  CHECK(expired.value_or(PresenceRow{}).source == PresenceSource::Timeout);
  CHECK(expired.value_or(PresenceRow{}).since == kStart + 2701);
  CHECK(rowOf(75, homeId()).value_or(PresenceRow{}).state ==
        PresenceState::Home);
  REQUIRE(harness.publisher.changes.size() == 1);
  CHECK(harness.publisher.changes.front().row.userId == 74);
  CHECK(harness.publisher.changes.front().overall == PresenceState::Away);

  GuardPresenceConfig shorter = testConfig();
  shorter.awayTimeoutSeconds = 600;
  harness.service.refresh(shorter);
  drogon::sync_wait(harness.service.sweep(kStart + 2601));
  CHECK(rowOf(75, homeId()).value_or(PresenceRow{}).source ==
        PresenceSource::Timeout);

  drogon::sync_wait(harness.service.sweep(kStart + 2701 + 2592001));
  CHECK_FALSE(rowOf(74, homeId()).has_value());
}

TEST_CASE("withdrawing consent or disabling the account deletes the rows at "
          "once, and other user changes are not consent changes")
{
  prepare();
  Harness harness;
  harness.directory.consenting = {76, 77};
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 76, .origin = "lan", .platform = "android", .at = kStart})));
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 77, .origin = "lan", .platform = "android", .at = kStart})));
  harness.publisher.changes.clear();

  Json::Value renamed(Json::objectValue);
  renamed["name"] = "Marta";
  renamed["isActive"] = true;
  drogon::sync_wait(harness.service.onIdentityChange(userChange(76, renamed)));
  CHECK(rowOf(76, homeId()).has_value());

  Json::Value withdrawn = renamed;
  withdrawn["privacy"]["decided"] = true;
  withdrawn["privacy"]["presence"] = false;
  drogon::sync_wait(harness.service.onIdentityChange(userChange(76, withdrawn)));
  CHECK_FALSE(rowOf(76, homeId()).has_value());
  REQUIRE(harness.publisher.changes.size() == 1);
  CHECK(harness.publisher.changes.front().row.state == PresenceState::Unknown);
  CHECK(harness.publisher.changes.front().row.source == PresenceSource::Consent);
  CHECK(harness.publisher.changes.front().overall == PresenceState::Unknown);

  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 76, .origin = "lan", .platform = "android", .at = kStart + 100})));
  CHECK_FALSE(rowOf(76, homeId()).has_value());

  Json::Value disabled = renamed;
  disabled["isActive"] = false;
  drogon::sync_wait(harness.service.onIdentityChange(userChange(77, disabled)));
  CHECK_FALSE(rowOf(77, homeId()).has_value());
  CHECK(harness.disabled == std::vector<int64_t>{77});

  Json::Value given = renamed;
  given["privacy"]["decided"] = true;
  given["privacy"]["presence"] = true;
  drogon::sync_wait(harness.service.onIdentityChange(userChange(76, given)));
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 76, .origin = "lan", .platform = "android", .at = kStart + 200})));
  CHECK(rowOf(76, homeId()).value_or(PresenceRow{}).since == kStart + 200);
}

TEST_CASE("the consent sweep removes whoever the directory no longer lists")
{
  prepare();
  Harness harness;
  harness.directory.consenting = {78, 79};
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 78, .origin = "lan", .platform = "android", .at = kStart})));
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 79, .origin = "lan", .platform = "android", .at = kStart})));

  harness.directory.consenting = {79};
  harness.directory.reachable = false;
  drogon::sync_wait(harness.service.reconcileConsent());
  CHECK(rowOf(78, homeId()).has_value());

  harness.directory.reachable = true;
  drogon::sync_wait(harness.service.reconcileConsent());
  CHECK_FALSE(rowOf(78, homeId()).has_value());
  CHECK(rowOf(79, homeId()).has_value());
}

TEST_CASE("the owner's view is coarse: a state and since when, per person and "
          "environment")
{
  prepare();
  const int64_t office = environmentNamed("Oficina vista", false);
  Harness harness;
  harness.directory.consenting = {80, 81};
  harness.directory.persons[910] = 80;
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 80, .origin = "tunnel", .platform = "android", .at = kStart})));
  drogon::sync_wait(
      harness.service.onKnownSeen(knownSeenEvent({.personId = 910, .environmentId = office, .passerby = false, .at = kStart + 10})));
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 81, .origin = "tunnel", .platform = "android", .at = kStart + 20})));

  const auto views = drogon::sync_wait(harness.service.snapshot({}));
  REQUIRE(views.size() == 2);
  CHECK(views.front().userId == 80);
  CHECK(views.front().overall == PresenceState::Home);
  CHECK(views.front().since == kStart + 10);
  CHECK(views.front().environments.size() == 2);
  CHECK(views.back().overall == PresenceState::Away);

  const Json::Value json = ResponsePresenceDto{.people = views}.toJson();
  REQUIRE(json["people"].size() == 2);
  CHECK(json["people"][0]["state"].asString() == "home");
  CHECK(json["people"][1]["state"].asString() == "away");
  CHECK_FALSE(json["people"][0].isMember("source"));
  CHECK_FALSE(json["people"][0]["environments"][0].isMember("source"));

  const auto one = drogon::sync_wait(harness.service.snapshot({81}));
  REQUIRE(one.size() == 1);
  CHECK(one.front().userId == 81);
}

TEST_CASE("the presence RPC answers only its callers, in the wire's terms")
{
  prepare();
  Harness harness;
  harness.directory.consenting = {82};
  drogon::sync_wait(harness.service.onSessionSignal(
      sessionEvent({.userId = 82, .origin = "lan", .platform = "android", .at = kStart})));

  PresenceRpcService rpc({.presence = &harness.service,
                          .credentials = {{.service = "sync",
                                           .secret = "sync-presence-secret"}}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&rpc);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const GuardPresenceClient stranger(
      {.target = target, .credential = "not-the-secret"});
  CHECK_FALSE(stranger.listPresence({}).has_value());

  const GuardPresenceClient sync(
      {.target = target, .credential = "sync-presence-secret"});
  const auto answer = sync.listPresence({82, 83});
  REQUIRE(answer.has_value());
  const auto response =
      answer.value_or(argus::guard::v1::ListPresenceResponse{});
  REQUIRE(response.users_size() == 1);
  CHECK(response.users(0).user_id() == 82);
  CHECK(response.users(0).overall() == argus::guard::v1::PRESENCE_STATE_HOME);
  CHECK(response.users(0).since() == kStart);
  REQUIRE(response.users(0).environments_size() == 1);
  CHECK(response.users(0).environments(0).source() ==
        argus::guard::v1::PRESENCE_SOURCE_LAN_SESSION);
  server->Shutdown();
}
