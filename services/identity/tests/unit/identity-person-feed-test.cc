#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <drogon/drogon.h>
#include <feature/person/services/person-feature-service.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/services/person-feed/person-feed.hxx>
#include <sqlite/db-service.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>
#include <sync/sync-operation.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "identity-person-feed-test.db";
constexpr int64_t kUserId = 7;
constexpr int64_t kOwnerId = 1;

using test_support::AppRunner;

std::set<std::string> householdFields()
{
  return {"id",          "userId",     "name",        "alias",
          "observation", "status",     "firstSeenAt", "lastSeenAt",
          "createdAt",   "updatedAt",  "deletedAt"};
}

bool waitForBoot()
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

std::set<std::string> fieldsOf(const Json::Value& row)
{
  const auto names = row.getMemberNames();
  return {names.begin(), names.end()};
}

class RecordingSink final : public IdentityChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override
  {
    emits_.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override
  {
    audits_.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    actions_.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishResource(const IdentityResourceInput&) const override
  {
    co_return;
  }

  [[nodiscard]] const std::vector<ModuleEmitInput>& emits() const { return emits_; }
  [[nodiscard]] const std::vector<ModuleAuditInput>& audits() const { return audits_; }
  [[nodiscard]] const std::vector<ActionPublishInput>& actions() const { return actions_; }

  void clear() const
  {
    emits_.clear();
    audits_.clear();
    actions_.clear();
  }

private:
  mutable std::vector<ModuleEmitInput> emits_;
  mutable std::vector<ModuleAuditInput> audits_;
  mutable std::vector<ActionPublishInput> actions_;
};

int64_t seedPerson(const std::optional<int64_t>& userId, const std::string& name)
{
  const auto rows = DbService::client()->execSqlSync(
      "INSERT INTO person (user_id, name, status, category, note, visitor_number, "
      "visit_count, first_seen_at, last_seen_at) "
      "VALUES (?, ?, 'candidate', 'neighbor', 'private note', ?, 3, 100, 100) "
      "RETURNING id",
      userId, name,
      userId ? std::optional<int64_t>{} : std::optional<int64_t>{12});
  return rows.front()["id"].as<int64_t>();
}

void removeDatabase()
{
  for (const char* suffix : {"", "-wal", "-shm"})
    std::remove((std::string(kDb) + suffix).c_str());
}
}

TEST_CASE("household persons travel live in their eleven fields, visitors never do")
{
  removeDatabase();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot());
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  DbService::client()->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active) "
      "VALUES (?, 'Ana', '', 'resident', 'es', 1)",
      kUserId);

  const int64_t household = seedPerson(kUserId, "Ana");
  const int64_t visitor = seedPerson(std::nullopt, "Mike");

  RecordingSink sink;
  identity_change::setSink(&sink);
  const PersonRepository persons;
  const PersonFeed feed;
  const PersonFeatureService service;

  {
    INFO("a household person is added with the full household row");
    sink.clear();
    const auto row = drogon::sync_wait(persons.findById(household));
    REQUIRE(row.has_value());
    drogon::sync_wait(feed.added({.person = row.value_or(PersonSchema{}), .client = nullptr}));
    REQUIRE(sink.emits().size() == 1);
    const auto& emit = sink.emits().front();
    CHECK(emit.table == TableName::Person);
    CHECK(emit.body.operation == SyncOperation::Add);
    CHECK(emit.body.option == TableName::Person);
    CHECK(fieldsOf(emit.body.obj) == householdFields());
    CHECK(emit.body.obj["id"].asInt64() == household);
    CHECK(emit.body.obj["userId"].asInt64() == kUserId);
    CHECK(emit.body.obj["name"].asString() == "Ana");
  }

  {
    INFO("a visitor is never added to the household stream");
    sink.clear();
    const auto row = drogon::sync_wait(persons.findById(visitor));
    REQUIRE(row.has_value());
    drogon::sync_wait(feed.added({.person = row.value_or(PersonSchema{}), .client = nullptr}));
    CHECK(sink.emits().empty());
  }

  {
    INFO("an observation on a household person is one diff inside its transaction");
    sink.clear();
    drogon::sync_wait(service.tag({.personId = household,
                                   .tags = {"companion"},
                                   .source = "guard",
                                   .observation = "accompanied by resident"}));
    CHECK(sink.emits().empty());
    REQUIRE(sink.audits().size() == 1);
    const auto& audit = sink.audits().front();
    CHECK(audit.tableName == TableName::Person);
    CHECK(audit.recordId == household);
    CHECK(audit.client != nullptr);
    CHECK_FALSE(audit.actorId.has_value());
    CHECK(fieldsOf(audit.before) == householdFields());
    CHECK(fieldsOf(audit.after) == householdFields());
    CHECK(audit.before["observation"].asString().empty());
    CHECK(audit.after["observation"].asString() == "accompanied by resident");
  }

  {
    INFO("a sighting of a household person carries its new last-seen time");
    sink.clear();
    CHECK(drogon::sync_wait(service.touch(household, 5000)));
    REQUIRE(sink.audits().size() == 1);
    const auto& audit = sink.audits().front();
    CHECK(audit.before["lastSeenAt"].asInt64() == 100);
    CHECK(audit.after["lastSeenAt"].asInt64() == 5000);
    CHECK(fieldsOf(audit.after) == householdFields());
  }

  {
    INFO("promoting a household person diffs the household projection only");
    sink.clear();
    const auto promoted =
        drogon::sync_wait(service.promote({.personId = household, .actorId = kOwnerId}));
    CHECK(promoted == std::optional<bool>{true});
    REQUIRE(sink.audits().size() == 1);
    const auto& audit = sink.audits().front();
    CHECK(audit.actorId == std::optional<int64_t>{kOwnerId});
    CHECK(audit.before["status"].asString() == "candidate");
    CHECK(audit.after["status"].asString() == "known");
    CHECK(fieldsOf(audit.after) == householdFields());
    CHECK(sink.actions().empty());
  }

  {
    INFO("a visitor's observation, sighting and promotion write no person frame");
    sink.clear();
    drogon::sync_wait(service.tag({.personId = visitor,
                                   .tags = {"companion"},
                                   .source = "guard",
                                   .observation = "accompanied by guest"}));
    CHECK(drogon::sync_wait(service.touch(visitor, 6000)));
    const auto promoted =
        drogon::sync_wait(service.promote({.personId = visitor, .actorId = kOwnerId}));
    CHECK(promoted == std::optional<bool>{true});
    CHECK(sink.emits().empty());
    CHECK(sink.audits().empty());
    CHECK(sink.actions().size() == 1);
  }

  {
    INFO("touching a person that does not exist writes nothing");
    sink.clear();
    CHECK_FALSE(drogon::sync_wait(service.touch(9999, 7000)));
    CHECK(sink.emits().empty());
    CHECK(sink.audits().empty());
  }

  identity_change::setSink(nullptr);
  removeDatabase();
}
