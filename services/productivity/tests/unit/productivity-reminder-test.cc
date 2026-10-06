#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/reminder/controllers/reminder-controller.hxx>
#include <feature/reminder/reminder-rpc-service.hxx>
#include <feature/reminder/services/reminder-feature-service.hxx>
#include <grpcpp/grpcpp.h>
#include <productivity/productivity-reminder-client.hxx>
#include <shared/repositories/reminder/reminder-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sync/sync-filter.hxx>
#include <sync/user-change-sink.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "productivity-reminder-test.db";
constexpr const char* kCredential = "productivity-reminder-test-credential";
constexpr int64_t kAna = 11;
constexpr int64_t kLuis = 12;
constexpr int64_t kOwner = 42;
constexpr int64_t kSoon = 1900000000;

using argus::productivity::v1::ReminderRow;

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

struct RecordedEmit
{
  SyncOperation operation{};
  TableName option{};
  Json::Value body;
  std::vector<int64_t> users;
};

struct RecordedAudit
{
  int64_t recordId{0};
  TableName tableName{};
  Json::Value before;
  Json::Value after;
  std::vector<int64_t> users;
};

class RecordingSink final : public UserChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    emits.push_back({.operation = input.body.operation,
                     .option = input.body.option,
                     .body = input.body.obj,
                     .users = input.userIds});
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    audits.push_back({.recordId = input.recordId,
                      .tableName = input.tableName,
                      .before = input.before,
                      .after = input.after,
                      .users = input.userIds});
    co_return;
  }

  void clear() const
  {
    const std::scoped_lock lock(mutex_);
    emits.clear();
    audits.clear();
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedEmit> emits;
  mutable std::vector<RecordedAudit> audits;
};

class RpcHarness
{
public:
  RpcHarness()
      : service_({argus::client::CallerCredential{.service = "argus-llm",
                                                  .secret = kCredential}})
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    target_ = "127.0.0.1:" + std::to_string(port);
  }

  ~RpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  [[nodiscard]] bool listening() const { return server_ != nullptr; }
  [[nodiscard]] const std::string& target() const { return target_; }

private:
  ReminderRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

drogon::HttpRequestPtr request(const Json::Value& body, int64_t sub, UserRole role)
{
  auto req = drogon::HttpRequest::newHttpJsonRequest(body);
  req->getAttributes()->insert(
      AuthContext::kJwtKey,
      JwtContext{.sub = sub,
                 .name = "Actor",
                 .role = role,
                 .isActive = true,
                 .deviceHash = {},
                 .sessionId = {}});
  return req;
}

struct Refusal
{
  int status{0};
  std::string code;
};

std::optional<Refusal> refusalOf(drogon::Task<drogon::HttpResponsePtr> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{.status = error.statusCode(), .code = error.errorCode()};
  }
  return std::nullopt;
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  REQUIRE(json);
  return (*json)["info"];
}

int64_t liveRows(const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM reminder WHERE title = ? "
      "AND deleted_at IS NULL",
      title);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

std::string columnOf(int64_t id, const std::string& column)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT " + column + " AS value FROM reminder WHERE id = ?", id);
  return rows.empty() || rows.front()["value"].isNull()
             ? std::string{}
             : rows.front()["value"].as<std::string>();
}

CreateReminderDto body(const std::string& title)
{
  CreateReminderDto dto;
  dto.title = title;
  dto.description = "kitchen";
  dto.scheduledAt = kSoon;
  return dto;
}

argus::client::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "owner", .device = "test-device"};
}

struct Fixture
{
  Fixture()
  {
    std::remove(kDb);
    std::remove((std::string(kDb) + "-wal").c_str());
    std::remove((std::string(kDb) + "-shm").c_str());
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = kDb,
                                   .name = "default",
                                   .timeout = -1});
  }

  AppRunner runner;
};

struct Env
{
  RecordingSink sink;
  ReminderFeatureService service;
  ReminderController controller;
};

void startScenario(Env& env)
{
  env.sink.clear();
  DbService::productivityClient()->execSqlSync("DELETE FROM reminder");
  DbService::productivityClient()->execSqlSync("DELETE FROM idempotency_key");
}

void createdReminderTargetsItsCreator(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderController& controller = env.controller;
  startScenario(env);

  Json::Value json;
  json["title"] = "Pills";
  json["description"] = "after lunch";
  json["scheduledAt"] = static_cast<Json::Int64>(kSoon);
  json["targetUserId"] = static_cast<Json::Int64>(kLuis);
  json["target_user_id"] = static_cast<Json::Int64>(kLuis);
  const auto response = drogon::sync_wait(
      controller.create(request(json, kAna, UserRole::Resident)));
  const Json::Value row = bodyOf(response);
  CHECK(row["targetUserId"].asInt64() == kAna);
  CHECK(row["createdBy"].asInt64() == kAna);
  CHECK(row["title"].asString() == "Pills");
  CHECK(columnOf(row["id"].asInt64(), "target_user_id") == std::to_string(kAna));

  REQUIRE(sink.emits.size() == 1);
  CHECK(sink.emits.front().operation == SyncOperation::Add);
  CHECK(sink.emits.front().option == TableName::Reminder);
  CHECK(sink.emits.front().users == std::vector<int64_t>{kAna});
  CHECK(sink.emits.front().body["id"].asInt64() == row["id"].asInt64());
}

void invalidBodiesAreRefused(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderController& controller = env.controller;
  startScenario(env);

  Json::Value json;
  json["description"] = "no title";
  json["scheduledAt"] = static_cast<Json::Int64>(kSoon);
  CHECK_THROWS_AS(drogon::sync_wait(controller.create(request(json, kAna, UserRole::Guest))),
                  ValidationException);
  json["title"] = "Late";
  json["scheduledAt"] = 0;
  CHECK_THROWS_AS(drogon::sync_wait(controller.create(request(json, kAna, UserRole::Guest))),
                  ValidationException);
  json["scheduledAt"] = static_cast<Json::Int64>(kSoon);
  json["title"] = std::string(201, 'x');
  CHECK_THROWS_AS(drogon::sync_wait(controller.create(request(json, kAna, UserRole::Guest))),
                  ValidationException);
  CHECK(sink.emits.empty());
}

void updatePublishesBeforeAndAfter(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderFeatureService& service = env.service;
  ReminderController& controller = env.controller;
  startScenario(env);

  const auto created = drogon::sync_wait(service.create(
      {.body = body("Water the plants"), .userId = kAna, .idempotencyKey = {}}));
  sink.clear();

  Json::Value patch;
  patch["title"] = "Water the ferns";
  patch["scheduledAt"] = static_cast<Json::Int64>(kSoon + 60);
  const auto response = drogon::sync_wait(controller.update(
      request(patch, kAna, UserRole::Resident), created.id));
  const Json::Value row = bodyOf(response);
  CHECK(row["title"].asString() == "Water the ferns");
  CHECK(row["scheduledAt"].asInt64() == kSoon + 60);
  CHECK(row["isCompleted"].asBool() == false);

  REQUIRE(sink.audits.size() == 1);
  CHECK(sink.audits.front().tableName == TableName::Reminder);
  CHECK(sink.audits.front().recordId == created.id);
  CHECK(sink.audits.front().before["title"].asString() == "Water the plants");
  CHECK(sink.audits.front().after["title"].asString() == "Water the ferns");
  CHECK(sink.audits.front().users == std::vector<int64_t>{kAna});
  CHECK(sink.emits.empty());
}

void completionStampsOnceAndReopeningClears(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderFeatureService& service = env.service;
  startScenario(env);

  const auto created = drogon::sync_wait(service.create(
      {.body = body("Call the plumber"), .userId = kAna, .idempotencyKey = {}}));
  sink.clear();

  UpdateReminderDto done;
  done.isCompleted = true;
  const auto completed = drogon::sync_wait(
      service.update({.id = created.id, .body = done, .userId = kAna}));
  if (!completed || !completed->completedAt) {
    FAIL("the completed reminder carries its time");
    return;
  }
  CHECK(completed->isCompleted);
  const int64_t stamp = completed->completedAt.value_or(0);
  CHECK(stamp > 0);

  const auto again = drogon::sync_wait(
      service.update({.id = created.id, .body = done, .userId = kAna}));
  CHECK(again.value_or(ReminderSchema{}).completedAt.value_or(0) == stamp);

  UpdateReminderDto reopen;
  reopen.isCompleted = false;
  const auto reopened = drogon::sync_wait(
      service.update({.id = created.id, .body = reopen, .userId = kAna}));
  const ReminderSchema reopenedRow = reopened.value_or(ReminderSchema{});
  CHECK(reopenedRow.id == created.id);
  CHECK_FALSE(reopenedRow.isCompleted);
  CHECK_FALSE(reopenedRow.completedAt.has_value());
  CHECK(sink.audits.size() == 2);
}

void emptyUpdatePublishesNothing(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderFeatureService& service = env.service;
  startScenario(env);

  const auto created = drogon::sync_wait(service.create(
      {.body = body("Idle"), .userId = kAna, .idempotencyKey = {}}));
  sink.clear();
  const UpdateReminderDto nothing;
  const auto row = drogon::sync_wait(
      service.update({.id = created.id, .body = nothing, .userId = kAna}));
  CHECK(row.has_value());
  CHECK(sink.audits.empty());
}

void deletionIsATombstoneForItsTarget(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderFeatureService& service = env.service;
  ReminderController& controller = env.controller;
  startScenario(env);

  const auto created = drogon::sync_wait(service.create(
      {.body = body("Throw away"), .userId = kAna, .idempotencyKey = {}}));
  sink.clear();

  const auto response = drogon::sync_wait(controller.remove(
      request(Json::Value(Json::objectValue), kAna, UserRole::Resident), created.id));
  CHECK(response->statusCode() == drogon::k204NoContent);

  REQUIRE(sink.emits.size() == 1);
  CHECK(sink.emits.front().operation == SyncOperation::Delete);
  CHECK(sink.emits.front().users == std::vector<int64_t>{kAna});
  CHECK(sink.emits.front().body["id"].asInt64() == created.id);
  CHECK(sink.emits.front().body.isMember("deletedAt"));
  CHECK(sink.emits.front().body.size() == 2);
  CHECK(liveRows("Throw away") == 0);
  CHECK_FALSE(drogon::sync_wait(service.get({.id = created.id, .userId = kAna})));

  const auto again = refusalOf(controller.remove(
      request(Json::Value(Json::objectValue), kAna, UserRole::Resident), created.id));
  CHECK(again.value_or(Refusal{}).status == 404);
}

void anotherUsersReminderAnswers404(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderFeatureService& service = env.service;
  ReminderController& controller = env.controller;
  startScenario(env);

  const auto mine = drogon::sync_wait(service.create(
      {.body = body("Ana only"), .userId = kAna, .idempotencyKey = {}}));
  sink.clear();

  struct Stranger
  {
    int64_t userId;
    UserRole role;
  };
  for (const Stranger stranger :
       {Stranger{.userId = kLuis, .role = UserRole::Resident},
        Stranger{.userId = kLuis, .role = UserRole::Guest},
        Stranger{.userId = kOwner, .role = UserRole::Owner}}) {
    Json::Value patch;
    patch["title"] = "hijacked";
    const auto update = refusalOf(controller.update(
        request(patch, stranger.userId, stranger.role), mine.id));
    const Refusal refusedUpdate = update.value_or(Refusal{});
    CHECK(refusedUpdate.status == 404);
    CHECK(refusedUpdate.code == "NOT_FOUND");

    const auto remove = refusalOf(controller.remove(
        request(Json::Value(Json::objectValue), stranger.userId, stranger.role), mine.id));
    CHECK(remove.value_or(Refusal{}).status == 404);

    CHECK_FALSE(drogon::sync_wait(
        service.get({.id = mine.id, .userId = stranger.userId})));
    CHECK(drogon::sync_wait(service.list({.userId = stranger.userId})).empty());
  }

  CHECK(columnOf(mine.id, "title") == "Ana only");
  CHECK(columnOf(mine.id, "deleted_at").empty());
  CHECK(sink.emits.empty());
  CHECK(sink.audits.empty());

  const auto read = drogon::sync_wait(service.get({.id = mine.id, .userId = kAna}));
  CHECK(read.value_or(ReminderSchema{}).title == "Ana only");
  CHECK(drogon::sync_wait(service.list({.userId = kAna})).size() == 1);
}

void repeatedKeyReturnsTheSameReminder(Env& env)
{
  RecordingSink& sink = env.sink;
  ReminderController& controller = env.controller;
  startScenario(env);

  Json::Value json;
  json["title"] = "Once";
  json["scheduledAt"] = static_cast<Json::Int64>(kSoon);
  auto first = request(json, kAna, UserRole::Resident);
  first->addHeader("Idempotency-Key", "retry-1");
  const auto one = bodyOf(drogon::sync_wait(controller.create(first)));
  auto second = request(json, kAna, UserRole::Resident);
  second->addHeader("Idempotency-Key", "retry-1");
  const auto two = bodyOf(drogon::sync_wait(controller.create(second)));
  CHECK(one["id"].asInt64() == two["id"].asInt64());
  CHECK(liveRows("Once") == 1);
  CHECK(sink.emits.size() == 1);

  auto other = request(json, kLuis, UserRole::Resident);
  other->addHeader("Idempotency-Key", "retry-1");
  const auto three = bodyOf(drogon::sync_wait(controller.create(other)));
  CHECK(three["id"].asInt64() != one["id"].asInt64());
  CHECK(three["targetUserId"].asInt64() == kLuis);
}

void syncListingsCarryOnlyTheCallersRows(Env& env)
{
  ReminderFeatureService& service = env.service;
  startScenario(env);

  const auto ana = drogon::sync_wait(service.create(
      {.body = body("Ana row"), .userId = kAna, .idempotencyKey = {}}));
  const auto luis = drogon::sync_wait(service.create(
      {.body = body("Luis row"), .userId = kLuis, .idempotencyKey = {}}));
  drogon::sync_wait(service.remove({.id = luis.id, .userId = kLuis}));
  DbService::productivityClient()->execSqlSync(
      "UPDATE reminder SET deleted_at = deleted_at - 5 WHERE id = ?", luis.id);

  const ReminderRepository repository;
  SyncFilter anaFilter;
  anaFilter.userId = kAna;
  SyncFilter luisFilter;
  luisFilter.userId = kLuis;
  SyncFilter nobody;

  const auto anaRows = drogon::sync_wait(repository.find(anaFilter));
  REQUIRE(anaRows.size() == 1);
  CHECK(anaRows.front()["id"].asInt64() == ana.id);
  CHECK(drogon::sync_wait(repository.find(luisFilter)).empty());
  CHECK(drogon::sync_wait(repository.find(nobody)).empty());

  CHECK(drogon::sync_wait(repository.findDeleted(anaFilter)).empty());
  const auto luisDeleted = drogon::sync_wait(repository.findDeleted(luisFilter));
  REQUIRE(luisDeleted.size() == 1);
  CHECK(luisDeleted.front()["id"].asInt64() == luis.id);
  CHECK(drogon::sync_wait(repository.findDeleted(nobody)).empty());

  const auto last = drogon::sync_wait(repository.findLast(anaFilter));
  CHECK(last.value_or(Json::Value())["id"].asInt64() == ana.id);
  CHECK_FALSE(drogon::sync_wait(repository.findLast(luisFilter)));
  CHECK_FALSE(drogon::sync_wait(repository.findLastDeleted(anaFilter)));
  CHECK(drogon::sync_wait(repository.findLastDeleted(luisFilter)));
}

void rpcActsForThePairedCallersUser(Env& env)
{
  startScenario(env);
  RecordingSink& sink = env.sink;
  RpcHarness harness;
  REQUIRE(harness.listening());
  ProductivityReminderClient client(
      {.target = harness.target(), .credential = kCredential});

  const auto created = client.create({.identity = identityFor(kAna),
                                      .title = "Via the assistant",
                                      .description = "spoken",
                                      .scheduledAt = kSoon,
                                      .recurrenceRule = std::nullopt,
                                      .idempotencyKey = {}});
  REQUIRE(created.outcome == ReminderRpcOutcome::Success);
  const auto createdRow = created.reminder.value_or(ReminderRow{});
  CHECK(createdRow.target_user_id() == kAna);
  CHECK(createdRow.title() == "Via the assistant");
  REQUIRE(sink.emits.size() == 1);
  CHECK(sink.emits.front().users == std::vector<int64_t>{kAna});
  const int64_t id = createdRow.id();

  const auto mine = client.get({.identity = identityFor(kAna), .id = id});
  CHECK(mine.outcome == ReminderRpcOutcome::Success);
  const auto theirs = client.get({.identity = identityFor(kLuis), .id = id});
  CHECK(theirs.outcome == ReminderRpcOutcome::NotFound);
  const auto owners = client.get({.identity = identityFor(kOwner), .id = id});
  CHECK(owners.outcome == ReminderRpcOutcome::NotFound);

  CHECK(client.update({.identity = identityFor(kLuis),
                       .id = id,
                       .title = "stolen",
                       .description = std::nullopt,
                       .scheduledAt = std::nullopt,
                       .isCompleted = std::nullopt})
            .outcome == ReminderRpcOutcome::NotFound);
  CHECK(client.remove({.identity = identityFor(kOwner), .id = id}) ==
        ReminderRpcOutcome::NotFound);
  CHECK(columnOf(id, "title") == "Via the assistant");

  const auto updated = client.update({.identity = identityFor(kAna),
                                      .id = id,
                                      .title = std::nullopt,
                                      .description = std::nullopt,
                                      .scheduledAt = std::nullopt,
                                      .isCompleted = true});
  REQUIRE(updated.outcome == ReminderRpcOutcome::Success);
  CHECK(updated.reminder.value_or(ReminderRow{}).is_completed());
  CHECK(sink.audits.size() == 1);

  const auto open = client.list({.identity = identityFor(kAna), .includeCompleted = false, .limit = 0});
  REQUIRE(open.outcome == ReminderRpcOutcome::Success);
  CHECK(open.reminders.empty());
  const auto all = client.list({.identity = identityFor(kAna), .includeCompleted = true, .limit = 0});
  CHECK(all.reminders.size() == 1);
  const auto others = client.list({.identity = identityFor(kLuis), .includeCompleted = true, .limit = 0});
  CHECK(others.reminders.empty());

  CHECK(client.create({.identity = identityFor(kAna),
                       .title = "",
                       .description = {},
                       .scheduledAt = kSoon,
                       .recurrenceRule = std::nullopt,
                       .idempotencyKey = {}})
            .outcome == ReminderRpcOutcome::Invalid);

  ProductivityReminderClient stranger(
      {.target = harness.target(), .credential = "not-the-assistant-secret"});
  CHECK(stranger.get({.identity = identityFor(kAna), .id = id}).outcome ==
        ReminderRpcOutcome::Refused);

  CHECK(client.remove({.identity = identityFor(kAna), .id = id}) ==
        ReminderRpcOutcome::Success);
  CHECK(client.get({.identity = identityFor(kAna), .id = id}).outcome ==
        ReminderRpcOutcome::NotFound);
  CHECK(sink.emits.back().operation == SyncOperation::Delete);
}
}

TEST_CASE("reminders belong to the caller, whoever asks, and every change is published")
{
  Fixture fixture;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  Env env;
  user_change::setProductivitySink(&env.sink);
  createdReminderTargetsItsCreator(env);
  invalidBodiesAreRefused(env);
  updatePublishesBeforeAndAfter(env);
  completionStampsOnceAndReopeningClears(env);
  emptyUpdatePublishesNothing(env);
  deletionIsATombstoneForItsTarget(env);
  anotherUsersReminderAnswers404(env);
  repeatedKeyReturnsTheSameReminder(env);
  syncListingsCarryOnlyTheCallersRows(env);
  rpcActsForThePairedCallersUser(env);
  user_change::setProductivitySink(nullptr);
}

