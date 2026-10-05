#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <argus/identity/v1/identity.grpc.pb.h>
#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <feature/calendar-event-share/controllers/calendar-event-share-controller.hxx>
#include <feature/calendar-event-share/dtos/create-calendar-event-share-dto.hxx>
#include <feature/calendar-event/controllers/calendar-event-controller.hxx>
#include <feature/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/project-member/controllers/project-member-controller.hxx>
#include <feature/project-member/dtos/create-project-member-dto.hxx>
#include <feature/project-task/controllers/project-task-controller.hxx>
#include <feature/project-task/dtos/create-project-task-dto.hxx>
#include <feature/project/controllers/project-controller.hxx>
#include <feature/project/dtos/create-project-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <sync/user-change-sink.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

#include <errors/response-exception.hxx>

#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>
#include <auth/request-context.hxx>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kProductivityDb = "productivity-controller-test.db";
constexpr const char* kTestSecret =
    "productivity-controller-test-secret-0123456789";

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    const std::string message = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error(message);
  }
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
  sqlite3_free(error);
}

namespace v1 = argus::identity::v1;

struct DirectoryUserInput
{
  int64_t userId{0};
  const char* name{""};
  const char* role{""};
  bool active{false};
};

class ScriptedUserDirectory final : public v1::IdentityService::CallbackService
{
public:
  ScriptedUserDirectory()
  {
    add({.userId = 42, .name = "Owner", .role = "owner", .active = true});
    add({.userId = 7, .name = "Resident", .role = "resident", .active = true});
    add({.userId = 9, .name = "Guard", .role = "guard", .active = true});
    add({.userId = 8, .name = "Inactive", .role = "resident", .active = false});
  }

  grpc::ServerUnaryReactor* GetUser(grpc::CallbackServerContext* context,
                                    const v1::GetUserRequest* request,
                                    v1::GetUserResponse* response) override
  {
    if (const auto found = users_.find(request->user_id());
        found != users_.end())
      *response->mutable_user() = found->second;
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

private:
  void add(const DirectoryUserInput& input)
  {
    v1::UserIdentity user;
    user.set_user_id(input.userId);
    user.set_name(input.name);
    user.set_last_name("Test");
    user.set_lang("es");
    user.set_role(input.role);
    user.set_is_active(input.active);
    users_.emplace(input.userId, std::move(user));
  }

  std::map<int64_t, v1::UserIdentity> users_;
};

class IdentityRpcHarness
{
public:
  IdentityRpcHarness()
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ConfigService::setRuntimeString("identity.target",
                                    "127.0.0.1:" + std::to_string(port));
  }

  ~IdentityRpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  bool listening() const { return server_ != nullptr; }

private:
  ScriptedUserDirectory service_;
  std::unique_ptr<grpc::Server> server_;
};

void seedProductivityDb(const std::string& path)
{
  std::remove(path.c_str());
  const auto db = openFile(path);
  exec(db.get(),
      "CREATE TABLE idempotency_key (user_id INTEGER NOT NULL, "
      "idem_key TEXT NOT NULL, route TEXT NOT NULL, record_id INTEGER NOT NULL, "
      "created_at INTEGER NOT NULL, PRIMARY KEY (user_id, idem_key))");
  exec(db.get(),
      "CREATE TABLE project ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', "
      "status TEXT NOT NULL DEFAULT 'active' "
      "CHECK (status IN ('planned', 'active', 'paused', 'done', 'canceled')), "
      "color TEXT NOT NULL DEFAULT '', starts_at INTEGER, target_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE TABLE project_task ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "project_id INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE, "
      "created_by INTEGER REFERENCES user(id) ON DELETE SET NULL, "
      "assignee_id INTEGER REFERENCES user(id) ON DELETE SET NULL, "
      "title TEXT NOT NULL, "
      "status TEXT NOT NULL DEFAULT 'todo' "
      "CHECK (status IN ('backlog', 'todo', 'doing', 'done', 'canceled')), "
      "priority TEXT NOT NULL DEFAULT 'none' "
      "CHECK (priority IN ('none', 'low', 'medium', 'high', 'urgent')), "
      "due_at INTEGER, sort_order REAL NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE TABLE calendar_event ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "created_by INTEGER REFERENCES user(id) ON DELETE SET NULL, "
      "owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "project_id INTEGER REFERENCES project(id) ON DELETE SET NULL, "
      "title TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', "
      "location TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '', "
      "starts_at INTEGER NOT NULL, "
      "ends_at INTEGER, "
      "is_all_day INTEGER NOT NULL DEFAULT 0 CHECK (is_all_day IN (0, 1)), "
      "recurrence_rule TEXT, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE TABLE project_member ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "project_id INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "access TEXT NOT NULL DEFAULT 'view' "
      "CHECK (access IN ('view', 'edit')), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE TABLE calendar_event_share ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "calendar_event_id INTEGER NOT NULL "
      "REFERENCES calendar_event(id) ON DELETE CASCADE, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "access TEXT NOT NULL DEFAULT 'view' "
      "CHECK (access IN ('view', 'edit')), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_project_member_unique "
      "ON project_member(project_id, user_id) WHERE deleted_at IS NULL");
  exec(db.get(),
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_calendar_event_share_unique "
      "ON calendar_event_share(calendar_event_id, user_id) "
      "WHERE deleted_at IS NULL");
}

struct RecordedEmit
{
  int operation{0};
  std::string option;
  Json::Value body;
  std::vector<int64_t> users;
};

struct RecordedAudit
{
  int64_t recordId{0};
  std::string tableName;
  std::string before;
  std::string after;
  std::vector<int64_t> users;
};

class RecordingSink final : public UserChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override
  {
    std::lock_guard lock(mutex_);
    emits.push_back({.operation = static_cast<int>(input.body.operation),
                     .option = tableNameToString(input.body.option),
                     .body = input.body.obj,
                     .users = input.userIds});
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override
  {
    std::lock_guard lock(mutex_);
    audits.push_back({input.recordId, tableNameToString(input.tableName),
                      json_util::toString(input.before),
                      json_util::toString(input.after), input.userIds});
    co_return;
  }

  mutable std::mutex mutex_;
  mutable std::vector<RecordedEmit> emits;
  mutable std::vector<RecordedAudit> audits;
};

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

void drain(const drogon::orm::DbClientPtr& client)
{
  auto drained = std::make_shared<std::promise<void>>();
  auto done = drained->get_future();
  client->execSqlAsync(
      "SELECT 1",
      [drained](const drogon::orm::Result&) {
        trantor::EventLoop::getEventLoopOfCurrentThread()->queueInLoop(
            [drained]() { drained->set_value(); });
      },
      [drained](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        }
        catch (const std::exception& ex) {
          std::fprintf(stderr, "drain statement failed: %s\n", ex.what());
        }
        drained->set_value();
      });
  if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    throw std::runtime_error("the client's loop did not drain");
}

Json::Value body(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  REQUIRE(json);
  return *json;
}

struct Refusal
{
  int status;
  std::string code;
  std::string message;
};

std::optional<Refusal> refusalOf(drogon::Task<drogon::HttpResponsePtr> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{error.statusCode(), error.errorCode(),
                   std::string(error.what())};
  }
  return std::nullopt;
}

struct SetActorInput
{
  const drogon::HttpRequestPtr& req;
  int64_t sub{0};
  UserRole role{};
};

void setActor(const SetActorInput& input)
{
  input.req->getAttributes()->insert(
      AuthContext::kJwtKey,
      JwtContext{.sub = input.sub, .name = "Actor", .role = input.role, .isActive = true, .deviceHash = {}, .sessionId = {}});
}

drogon::HttpRequestPtr ownerRequest(int64_t sub = 42)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  setActor({.req = req, .sub = sub, .role = UserRole::Owner});
  return req;
}
}

TEST_CASE("productivity contracts hold on the argus-productivity surface")
{
  seedProductivityDb(kProductivityDb);
  ConfigService::setRuntimeString("jwt.secret", kTestSecret);
  ConfigService::setRuntimeString("jwt.refresh_secret", kTestSecret);

  IdentityRpcHarness identity;
  REQUIRE(identity.listening());

  drogon::app().setLogLevel(trantor::Logger::kWarn);

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  auto productivityDb = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + kProductivityDb, 1);
  DbService::setProductivityClient(productivityDb);

  RecordingSink sink;
  user_change::setProductivitySink(&sink);

  ProjectController projectController;

  Json::Value projectBody;
  projectBody["name"] = "Renovation";
  projectBody["description"] = "Fix the gate";
  projectBody["status"] = "active";
  projectBody["color"] = "#00FF00";
  projectBody["startsAt"] = Json::Int64(1735689600000);
  auto projectReq = drogon::HttpRequest::newHttpJsonRequest(projectBody);
  projectReq->addHeader("Idempotency-Key", "project-retry-1");
  setActor({.req = projectReq, .sub = 42, .role = UserRole::Owner});
  const auto created = drogon::sync_wait(projectController.create(projectReq));
  REQUIRE(created);
  const Json::Value projectJson = body(created);
  CHECK(projectJson["status"].asInt() == 200);
  CHECK(projectJson["errors"].isNull());
  const Json::Value project = projectJson["info"];
  const int64_t projectId = project["id"].asInt64();
  CHECK(projectId > 0);
  CHECK(project["ownerId"].asInt64() == 42);
  CHECK(project["name"] == "Renovation");
  CHECK(project["description"] == "Fix the gate");
  CHECK(project["status"] == "active");
  CHECK(project["color"] == "#00FF00");
  CHECK(project["startsAt"].asInt64() == 1735689600000);
  CHECK(project["targetAt"].isNull());
  CHECK(project["updatedAt"].isNull());
  CHECK(project["deletedAt"].isNull());
  CHECK(project.getMemberNames().size() == 11);
  REQUIRE(sink.emits.size() == 1);
  CHECK(sink.emits.front().operation
        == static_cast<int>(SyncOperation::Add));
  CHECK(sink.emits.front().option == "project");
  CHECK(sink.emits.front().body["id"].asInt64() == projectId);

  auto retriedReq = drogon::HttpRequest::newHttpJsonRequest(projectBody);
  retriedReq->addHeader("Idempotency-Key", "project-retry-1");
  setActor({.req = retriedReq, .sub = 42, .role = UserRole::Owner});
  const auto retried = drogon::sync_wait(projectController.create(retriedReq));
  CHECK(body(retried)["info"]["id"].asInt64() == projectId);
  CHECK(sink.emits.size() == 1);

  auto badKeyReq = drogon::HttpRequest::newHttpJsonRequest(projectBody);
  badKeyReq->addHeader("Idempotency-Key", "not a key!");
  setActor({.req = badKeyReq, .sub = 42, .role = UserRole::Owner});
  CHECK_THROWS(drogon::sync_wait(projectController.create(badKeyReq)));

  auto missingUpdateReq =
      drogon::HttpRequest::newHttpJsonRequest(projectBody);
  setActor({.req = missingUpdateReq, .sub = 42, .role = UserRole::Owner});
  const auto missingUpdate =
      refusalOf(projectController.update(missingUpdateReq, 999));
  if (!missingUpdate) {
    FAIL("expected a value in missingUpdate");
    return;
  }
  CHECK(missingUpdate->status == 404);
  CHECK(missingUpdate->code == "NOT_FOUND");
  CHECK(missingUpdate->message == "Project not found");

  Json::Value renameBody;
  renameBody["name"] = "Renovation 2";
  auto renameReq = drogon::HttpRequest::newHttpJsonRequest(renameBody);
  setActor({.req = renameReq, .sub = 42, .role = UserRole::Owner});
  const auto renamed = drogon::sync_wait(projectController.update(renameReq,
                                                                  projectId));
  const Json::Value renamedJson = body(renamed);
  CHECK(renamedJson["status"].asInt() == 200);
  CHECK(renamedJson["info"]["name"] == "Renovation 2");
  CHECK(renamedJson["info"]["updatedAt"].asInt64() > 0);
  REQUIRE(sink.audits.size() == 1);
  CHECK(sink.audits.front().recordId == projectId);
  CHECK(sink.audits.front().tableName == "project");
  CHECK(json_util::fromString(sink.audits.front().before)["name"]
        == "Renovation");
  CHECK(json_util::fromString(sink.audits.front().after)["name"]
        == "Renovation 2");
  CHECK(sink.audits.front().users == std::vector<int64_t>{42});

  ProjectMemberController memberController;

  auto memberReq = [&](int64_t userId, const char* access) {
    Json::Value memberBody;
    memberBody["projectId"] = Json::Int64(projectId);
    memberBody["userId"] = Json::Int64(userId);
    memberBody["access"] = access;
    auto req = drogon::HttpRequest::newHttpJsonRequest(memberBody);
    setActor({.req = req, .sub = 42, .role = UserRole::Owner});
    return req;
  };

  const auto selfShare =
      refusalOf(memberController.create(memberReq(42, "view")));
  if (!selfShare) {
    FAIL("expected a value in selfShare");
    return;
  }
  CHECK(selfShare->status == 409);
  CHECK(selfShare->message == "The owner already has access");

  const auto inactiveShare =
      refusalOf(memberController.create(memberReq(8, "view")));
  if (!inactiveShare) {
    FAIL("expected a value in inactiveShare");
    return;
  }
  CHECK(inactiveShare->status == 404);
  CHECK(inactiveShare->message == "User not found");

  const auto guardShare =
      refusalOf(memberController.create(memberReq(9, "view")));
  if (!guardShare) {
    FAIL("expected a value in guardShare");
    return;
  }
  CHECK(guardShare->status == 403);
  CHECK(guardShare->message == "That user cannot see projects");

  const auto memberCreated =
      drogon::sync_wait(memberController.create(memberReq(7, "view")));
  const Json::Value memberJson = body(memberCreated);
  CHECK(memberJson["status"].asInt() == 200);
  CHECK(memberJson["errors"].isNull());
  const Json::Value member = memberJson["info"];
  const int64_t memberId = member["id"].asInt64();
  CHECK(memberId > 0);
  CHECK(member["projectId"].asInt64() == projectId);
  CHECK(member["userId"].asInt64() == 7);
  CHECK(member["access"] == "view");
  CHECK(member.getMemberNames().size() == 7);
  REQUIRE(sink.emits.size() == 2);
  CHECK(sink.emits.at(1).option == "project_member");
  CHECK(sink.emits.at(1).users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.at(1).body["id"].asInt64() == memberId);

  Json::Value editBody;
  editBody["access"] = "edit";
  auto editReq = drogon::HttpRequest::newHttpJsonRequest(editBody);
  setActor({.req = editReq, .sub = 42, .role = UserRole::Owner});
  const auto memberEdited =
      drogon::sync_wait(memberController.update(editReq, memberId));
  CHECK(body(memberEdited)["status"].asInt() == 200);
  CHECK(body(memberEdited)["info"]["access"] == "edit");
  REQUIRE(sink.audits.size() == 2);
  CHECK(sink.audits.back().recordId == memberId);
  CHECK(sink.audits.back().tableName == "project_member");
  CHECK(sink.audits.back().users == std::vector<int64_t>{42, 7});

  const auto memberGone =
      drogon::sync_wait(memberController.remove(ownerRequest(), memberId));
  CHECK(body(memberGone)["status"].asInt() == 200);
  CHECK(body(memberGone)["info"]["deleted"].asBool());
  CHECK(body(memberGone)["info"]["id"].asInt64() == memberId);
  const auto memberGoneTwice =
      refusalOf(memberController.remove(ownerRequest(), memberId));
  if (!memberGoneTwice) {
    FAIL("expected a value in memberGoneTwice");
    return;
  }
  CHECK(memberGoneTwice->status == 404);
  CHECK(memberGoneTwice->message == "Share not found");
  REQUIRE(sink.emits.size() == 3);
  CHECK(sink.emits.at(2).operation
        == static_cast<int>(SyncOperation::Delete));
  CHECK(sink.emits.at(2).option == "project_member");
  CHECK(sink.emits.at(2).users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.at(2).body["id"].asInt64() == memberId);

  ProjectTaskController taskController;

  Json::Value taskBody;
  taskBody["projectId"] = Json::Int64(projectId);
  taskBody["title"] = "Sand the door";
  taskBody["status"] = "todo";
  taskBody["priority"] = "low";
  taskBody["sortOrder"] = 1.0;
  auto reusedKeyReq = drogon::HttpRequest::newHttpJsonRequest(taskBody);
  reusedKeyReq->addHeader("Idempotency-Key", "project-retry-1");
  setActor({.req = reusedKeyReq, .sub = 42, .role = UserRole::Owner});
  const auto reusedKey = refusalOf(taskController.create(reusedKeyReq));
  if (!reusedKey) {
    FAIL("expected a value in reusedKey");
    return;
  }
  CHECK(reusedKey->status == 409);

  auto taskReq = drogon::HttpRequest::newHttpJsonRequest(taskBody);
  setActor({.req = taskReq, .sub = 42, .role = UserRole::Owner});
  const auto taskCreated = drogon::sync_wait(taskController.create(taskReq));
  const Json::Value taskJson = body(taskCreated);
  CHECK(taskJson["status"].asInt() == 200);
  CHECK(taskJson["errors"].isNull());
  const Json::Value task = taskJson["info"];
  const int64_t taskId = task["id"].asInt64();
  CHECK(taskId > 0);
  CHECK(task["projectId"].asInt64() == projectId);
  CHECK(task["createdBy"].asInt64() == 42);
  CHECK(task["title"] == "Sand the door");
  CHECK(task["status"] == "todo");
  CHECK(task["priority"] == "low");
  CHECK(task["sortOrder"].asDouble() == 1.0);
  CHECK(task["assigneeId"].isNull());
  CHECK(task["dueAt"].isNull());
  CHECK(task.getMemberNames().size() == 12);
  REQUIRE(sink.emits.size() == 4);
  CHECK(sink.emits.back().option == "project_task");
  CHECK(sink.emits.back().users == std::vector<int64_t>{42});

  const auto taskMissing =
      refusalOf(taskController.remove(ownerRequest(), 999));
  if (!taskMissing) {
    FAIL("expected a value in taskMissing");
    return;
  }
  CHECK(taskMissing->status == 404);
  CHECK(taskMissing->message == "Task not found");

  Json::Value taskStatusBody;
  taskStatusBody["status"] = "doing";
  auto taskUpdateReq =
      drogon::HttpRequest::newHttpJsonRequest(taskStatusBody);
  setActor({.req = taskUpdateReq, .sub = 42, .role = UserRole::Owner});
  const auto taskMoved =
      drogon::sync_wait(taskController.update(taskUpdateReq, taskId));
  CHECK(body(taskMoved)["status"].asInt() == 200);
  CHECK(body(taskMoved)["info"]["status"] == "doing");
  REQUIRE(sink.audits.size() == 3);
  CHECK(sink.audits.back().recordId == taskId);
  CHECK(sink.audits.back().tableName == "project_task");
  CHECK(sink.audits.back().users == std::vector<int64_t>{42});

  const auto patchTask = [&taskController, taskId](const std::string& json) {
    auto req = drogon::HttpRequest::newHttpJsonRequest(json_util::fromString(json));
    setActor({.req = req, .sub = 42, .role = UserRole::Owner});
    return body(drogon::sync_wait(taskController.update(req, taskId)))["info"];
  };
  CHECK(patchTask(R"({"dueAt":1790000000})")["dueAt"].asInt64() == 1790000000);
  CHECK(patchTask(R"({"title":"Sand the door twice"})")["dueAt"].asInt64() == 1790000000);
  CHECK(patchTask(R"({"dueAt":null})")["dueAt"].isNull());

  Json::Value orphanTaskBody;
  orphanTaskBody["projectId"] = Json::Int64(999);
  orphanTaskBody["title"] = "Orphan";
  orphanTaskBody["status"] = "todo";
  orphanTaskBody["priority"] = "none";
  auto orphanReq = drogon::HttpRequest::newHttpJsonRequest(orphanTaskBody);
  setActor({.req = orphanReq, .sub = 42, .role = UserRole::Owner});
  const auto orphan = refusalOf(taskController.create(orphanReq));
  if (!orphan) {
    FAIL("expected a value in orphan");
    return;
  }
  CHECK(orphan->status == 404);
  CHECK(orphan->message == "Project not found");

  const std::size_t emitsBeforeRegrant = sink.emits.size();
  const auto regranted =
      drogon::sync_wait(memberController.create(memberReq(7, "view")));
  const int64_t regrantedId = body(regranted)["info"]["id"].asInt64();
  REQUIRE(sink.emits.size() == emitsBeforeRegrant + 1);
  CHECK(sink.emits.back().option == "project_member");
  CHECK(sink.emits.back().operation == static_cast<int>(SyncOperation::Add));
  CHECK(sink.emits.back().users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.back().body["id"].asInt64() == regrantedId);
  CHECK(sink.emits.back().body["projectId"].asInt64() == projectId);

  drogon::sync_wait(memberController.remove(ownerRequest(), regrantedId));
  REQUIRE(sink.emits.size() == emitsBeforeRegrant + 2);
  CHECK(sink.emits.back().option == "project_member");
  CHECK(sink.emits.back().operation == static_cast<int>(SyncOperation::Delete));
  CHECK(sink.emits.back().users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.back().body["id"].asInt64() == regrantedId);

  CalendarEventController eventController;

  Json::Value strayEventBody;
  strayEventBody["title"] = "Stray";
  strayEventBody["startsAt"] = Json::Int64(1735689600000);
  strayEventBody["projectId"] = Json::Int64(999);
  auto strayEventReq = drogon::HttpRequest::newHttpJsonRequest(strayEventBody);
  setActor({.req = strayEventReq, .sub = 42, .role = UserRole::Owner});
  const std::size_t emitsBeforeStray = sink.emits.size();
  const auto stray = refusalOf(eventController.create(strayEventReq));
  if (!stray) {
    FAIL("expected a value in stray");
    return;
  }
  CHECK(stray->status == 404);
  CHECK(stray->message == "Project not found");
  CHECK(sink.emits.size() == emitsBeforeStray);

  Json::Value eventBody;
  eventBody["title"] = "Gate review";
  eventBody["description"] = "Walk the gate";
  eventBody["location"] = "Front gate";
  eventBody["color"] = "#0000FF";
  eventBody["startsAt"] = Json::Int64(1735689600000);
  eventBody["endsAt"] = Json::Int64(1735693200000);
  eventBody["isAllDay"] = false;
  eventBody["projectId"] = Json::Int64(projectId);
  auto eventReq = drogon::HttpRequest::newHttpJsonRequest(eventBody);
  eventReq->addHeader("Idempotency-Key", "event-retry-1");
  setActor({.req = eventReq, .sub = 42, .role = UserRole::Owner});
  const auto eventCreated = drogon::sync_wait(eventController.create(eventReq));
  const Json::Value eventJson = body(eventCreated);
  CHECK(eventJson["status"].asInt() == 200);
  CHECK(eventJson["errors"].isNull());
  const Json::Value event = eventJson["info"];
  const int64_t eventId = event["id"].asInt64();
  CHECK(eventId > 0);
  CHECK(event["ownerId"].asInt64() == 42);
  CHECK(event["createdBy"].asInt64() == 42);
  CHECK(event["projectId"].asInt64() == projectId);
  CHECK(event["title"] == "Gate review");
  CHECK(event["startsAt"].asInt64() == 1735689600000);
  CHECK(event["endsAt"].asInt64() == 1735693200000);
  CHECK_FALSE(event["isAllDay"].asBool());
  CHECK(event["recurrenceRule"].isNull());
  CHECK(event.getMemberNames().size() == 15);
  REQUIRE(sink.emits.size() == 7);
  CHECK(sink.emits.back().option == "calendar_event");
  CHECK(sink.emits.back().users == std::vector<int64_t>{42});

  auto eventRetryReq = drogon::HttpRequest::newHttpJsonRequest(eventBody);
  eventRetryReq->addHeader("Idempotency-Key", "event-retry-1");
  setActor({.req = eventRetryReq, .sub = 42, .role = UserRole::Owner});
  const auto eventRetried = drogon::sync_wait(eventController.create(eventRetryReq));
  CHECK(body(eventRetried)["info"]["id"].asInt64() == eventId);
  CHECK(sink.emits.size() == 7);

  CalendarEventShareController shareController;

  auto shareReq = [&](int64_t userId) {
    Json::Value shareBody;
    shareBody["calendarEventId"] = Json::Int64(eventId);
    shareBody["userId"] = Json::Int64(userId);
    shareBody["access"] = "view";
    auto req = drogon::HttpRequest::newHttpJsonRequest(shareBody);
    setActor({.req = req, .sub = 42, .role = UserRole::Owner});
    return req;
  };

  const auto eventSelfShare =
      refusalOf(shareController.create(shareReq(42)));
  if (!eventSelfShare) {
    FAIL("expected a value in eventSelfShare");
    return;
  }
  CHECK(eventSelfShare->status == 409);
  const auto eventGuardShare =
      refusalOf(shareController.create(shareReq(9)));
  if (!eventGuardShare) {
    FAIL("expected a value in eventGuardShare");
    return;
  }
  CHECK(eventGuardShare->status == 403);
  CHECK(eventGuardShare->message == "That user cannot see calendar events");

  const auto shareCreated = drogon::sync_wait(shareController.create(shareReq(7)));
  const Json::Value shareJson = body(shareCreated);
  CHECK(shareJson["status"].asInt() == 200);
  CHECK(shareJson["errors"].isNull());
  const Json::Value share = shareJson["info"];
  const int64_t shareId = share["id"].asInt64();
  CHECK(shareId > 0);
  CHECK(share["calendarEventId"].asInt64() == eventId);
  CHECK(share["userId"].asInt64() == 7);
  CHECK(share["access"] == "view");
  CHECK(share.getMemberNames().size() == 7);
  REQUIRE(sink.emits.size() == 8);
  CHECK(sink.emits.back().option == "calendar_event_share");
  CHECK(sink.emits.back().users == std::vector<int64_t>{42, 7});

  Json::Value shareEditBody;
  shareEditBody["access"] = "edit";
  auto shareEditReq = drogon::HttpRequest::newHttpJsonRequest(shareEditBody);
  setActor({.req = shareEditReq, .sub = 42, .role = UserRole::Owner});
  const auto shareEdited =
      drogon::sync_wait(shareController.update(shareEditReq, shareId));
  CHECK(body(shareEdited)["status"].asInt() == 200);
  CHECK(body(shareEdited)["info"]["access"] == "edit");
  REQUIRE(sink.audits.size() == 7);
  CHECK(sink.audits.back().recordId == shareId);
  CHECK(sink.audits.back().tableName == "calendar_event_share");
  CHECK(sink.audits.back().users == std::vector<int64_t>{42, 7});

  const auto shareGone =
      drogon::sync_wait(shareController.remove(ownerRequest(), shareId));
  CHECK(body(shareGone)["status"].asInt() == 200);
  CHECK(body(shareGone)["info"]["id"].asInt64() == shareId);
  const auto shareGoneTwice =
      refusalOf(shareController.remove(ownerRequest(), shareId));
  if (!shareGoneTwice) {
    FAIL("expected a value in shareGoneTwice");
    return;
  }
  CHECK(shareGoneTwice->status == 404);
  CHECK(shareGoneTwice->message == "Share not found");

  const auto patchEvent = [&eventController, eventId](const std::string& json) {
    auto req = drogon::HttpRequest::newHttpJsonRequest(json_util::fromString(json));
    setActor({.req = req, .sub = 42, .role = UserRole::Owner});
    return body(drogon::sync_wait(eventController.update(req, eventId)))["info"];
  };
  const auto located = patchEvent(R"({"location":"Porch","endsAt":1735699600000})");
  CHECK(located["location"] == "Porch");
  CHECK(located["endsAt"].asInt64() == 1735699600000);
  const auto untouched = patchEvent(R"({"title":"Golden event, moved"})");
  CHECK(untouched["location"] == "Porch");
  CHECK(untouched["endsAt"].asInt64() == 1735699600000);
  const auto cleared = patchEvent(R"({"location":null,"description":null,"endsAt":null})");
  CHECK(cleared["location"] == "");
  CHECK(cleared["description"] == "");
  CHECK(cleared["endsAt"].isNull());
  CHECK(cleared["projectId"].asInt64() == projectId);
  const auto nullProject = patchEvent(R"({"projectId":null,"title":"Still linked"})");
  CHECK(nullProject["title"] == "Still linked");
  CHECK(nullProject["projectId"].asInt64() == projectId);

  Json::Value zeroProject;
  zeroProject["projectId"] = static_cast<Json::Int64>(0);
  auto zeroProjectReq = drogon::HttpRequest::newHttpJsonRequest(zeroProject);
  setActor({.req = zeroProjectReq, .sub = 42, .role = UserRole::Owner});
  const auto zeroRefused = refusalOf(eventController.update(zeroProjectReq, eventId));
  if (!zeroRefused) {
    FAIL("expected a value in zeroRefused");
    return;
  }
  CHECK(zeroRefused->status == 404);
  CHECK(zeroRefused->message == "Project not found");
  CHECK(patchEvent(R"({"title":"Still linked"})")["projectId"].asInt64() == projectId);

  const auto eventGone =
      drogon::sync_wait(eventController.remove(ownerRequest(), eventId));
  CHECK(body(eventGone)["status"].asInt() == 200);
  CHECK(body(eventGone)["info"]["deleted"].asBool());
  CHECK(body(eventGone)["info"]["id"].asInt64() == eventId);
  const auto eventGoneTwice =
      refusalOf(eventController.remove(ownerRequest(), eventId));
  if (!eventGoneTwice) {
    FAIL("expected a value in eventGoneTwice");
    return;
  }
  CHECK(eventGoneTwice->status == 404);
  CHECK(eventGoneTwice->message == "Calendar event not found");

  const std::size_t emitsBeforeProjectGone = sink.emits.size();
  const auto projectGone =
      drogon::sync_wait(projectController.remove(ownerRequest(), projectId));
  CHECK(body(projectGone)["status"].asInt() == 200);
  CHECK(body(projectGone)["info"]["deleted"].asBool());
  CHECK(body(projectGone)["info"]["id"].asInt64() == projectId);
  REQUIRE(sink.emits.size() == emitsBeforeProjectGone + 2);
  CHECK(sink.emits.at(emitsBeforeProjectGone).option == "project_task");
  CHECK(sink.emits.at(emitsBeforeProjectGone).operation ==
        static_cast<int>(SyncOperation::Delete));
  CHECK(sink.emits.at(emitsBeforeProjectGone).body["id"].asInt64() == taskId);
  CHECK(sink.emits.back().option == "project");
  const auto orphanedTask = refusalOf(taskController.remove(ownerRequest(), taskId));
  if (!orphanedTask) {
    FAIL("expected a value in orphanedTask");
    return;
  }
  CHECK(orphanedTask->status == 404);

  user_change::setProductivitySink(nullptr);
  DbService::setProductivityClient(nullptr);

  drain(productivityDb);
  productivityDb.reset();

  std::remove(kProductivityDb);
  std::remove((std::string(kProductivityDb) + "-wal").c_str());
  std::remove((std::string(kProductivityDb) + "-shm").c_str());
}
