#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <drogon/drogon.h>
#include <feature/api/calendar-event-share/controllers/calendar-event-share-controller.hxx>
#include <feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.hxx>
#include <feature/api/calendar-event/controllers/calendar-event-controller.hxx>
#include <feature/api/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/api/project-member/controllers/project-member-controller.hxx>
#include <feature/api/project-member/dtos/create-project-member-dto.hxx>
#include <feature/api/project-task/controllers/project-task-controller.hxx>
#include <feature/api/project-task/dtos/create-project-task-dto.hxx>
#include <feature/api/project/controllers/project-controller.hxx>
#include <feature/api/project/dtos/create-project-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kIdentityDb = "productivity-controller-test-identity.db";
constexpr const char* kProductivityDb = "productivity-controller-test.db";

void seedIdentityDb(const char* path)
{
  std::remove(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE user ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL, last_name TEXT NOT NULL, "
      "role TEXT NOT NULL CHECK (role IN ('owner', 'resident', 'guard', "
      "'guest')), lang TEXT NOT NULL DEFAULT 'es' "
      "CHECK (lang IN ('es', 'en')), "
      "is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  for (const auto& [id, name, role, isActive] :
       std::vector<std::tuple<int64_t, const char*, const char*,
                              int>>{{42, "Owner", "owner", 1},
                                    {7, "Resident", "resident", 1},
                                    {9, "Guard", "guard", 1},
                                    {8, "Inactive", "resident", 0}}) {
    client->execSqlSync(
        "INSERT INTO user (id, name, last_name, role, is_active) "
        "VALUES (?, ?, 'Test', ?, ?)",
        id, name, role, isActive);
  }
}

// Seeds the five write-domain tables and the sharing indexes.
void seedProductivityDb(const char* path)
{
  std::remove(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
                                              1);
  client->execSqlSync(
      "CREATE TABLE project ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', "
      "status TEXT NOT NULL DEFAULT 'active' "
      "CHECK (status IN ('planned', 'active', 'paused', 'done', 'canceled')), "
      "color TEXT NOT NULL DEFAULT '', starts_at INTEGER, target_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
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
  client->execSqlSync(
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
  client->execSqlSync(
      "CREATE TABLE project_member ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "project_id INTEGER NOT NULL REFERENCES project(id) ON DELETE CASCADE, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "access TEXT NOT NULL DEFAULT 'view' "
      "CHECK (access IN ('view', 'edit')), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
      "CREATE TABLE calendar_event_share ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "calendar_event_id INTEGER NOT NULL "
      "REFERENCES calendar_event(id) ON DELETE CASCADE, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "access TEXT NOT NULL DEFAULT 'view' "
      "CHECK (access IN ('view', 'edit')), "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_project_member_unique "
      "ON project_member(project_id, user_id) WHERE deleted_at IS NULL");
  client->execSqlSync(
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

// Records the emits and audit diffs the feature services hand to the funnel.
class RecordingSink final : public UserChangeSink
{
public:
  void emitUser(int64_t userId, const SocketEmitDto& body) const override
  {
    emitUsers({userId}, body);
  }

  void emitUsers(const std::vector<int64_t>& userIds,
                 const SocketEmitDto& body) const override
  {
    std::lock_guard lock(mutex_);
    emits.push_back({static_cast<int>(body.operation),
                     tableNameToString(body.option), body.obj, userIds});
  }

  drogon::Task<void> publishAudit(const UserAuditInput& input) const override
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

Json::Value body(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  REQUIRE(json);
  return *json;
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
      AppConfig::JWT_CTX_KEY,
      JwtContext{input.sub, "Actor", input.role, true, {}});
}

// Delete endpoints read the actor from the request attributes the filters leave.
drogon::HttpRequestPtr ownerRequest(int64_t sub = 42)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  setActor({.req = req, .sub = sub, .role = UserRole::Owner});
  return req;
}
} // namespace

TEST_CASE("productivity contracts hold on the argus-productivity surface")
{
  seedIdentityDb(kIdentityDb);
  seedProductivityDb(kProductivityDb);
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kIdentityDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  const auto productivityDb = drogon::orm::DbClient::newSqlite3Client(
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

  auto missingUpdateReq =
      drogon::HttpRequest::newHttpJsonRequest(projectBody);
  setActor({.req = missingUpdateReq, .sub = 42, .role = UserRole::Owner});
  const auto missingUpdate =
      drogon::sync_wait(projectController.update(missingUpdateReq, 999));
  const Json::Value missingUpdateJson = body(missingUpdate);
  CHECK(missingUpdateJson["status"].asInt() == 404);
  CHECK(missingUpdateJson["info"].isNull());
  CHECK(missingUpdateJson["errors"]["code"] == "NOT_FOUND");
  CHECK(missingUpdateJson["errors"]["message"] == "Project not found");

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

  const auto selfShare = drogon::sync_wait(
      memberController.create(memberReq(42, "view")));
  CHECK(body(selfShare)["status"].asInt() == 409);
  CHECK(body(selfShare)["errors"]["message"] == "The owner already has access");

  const auto inactiveShare = drogon::sync_wait(
      memberController.create(memberReq(8, "view")));
  CHECK(body(inactiveShare)["status"].asInt() == 404);
  CHECK(body(inactiveShare)["errors"]["message"] == "User not found");

  const auto guardShare = drogon::sync_wait(
      memberController.create(memberReq(9, "view")));
  CHECK(body(guardShare)["status"].asInt() == 403);
  CHECK(body(guardShare)["errors"]["message"]
        == "That user cannot see projects");

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
  REQUIRE(sink.emits.size() == 3);
  CHECK(sink.emits.at(1).option == "project_member");
  CHECK(sink.emits.at(1).users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.at(1).body["id"].asInt64() == memberId);
  CHECK(sink.emits.at(2).option == "project");
  CHECK(sink.emits.at(2).users == std::vector<int64_t>{7});

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
  const auto memberGoneTwice =
      drogon::sync_wait(memberController.remove(ownerRequest(), memberId));
  CHECK(body(memberGoneTwice)["status"].asInt() == 404);
  CHECK(body(memberGoneTwice)["errors"]["message"] == "Share not found");
  REQUIRE(sink.emits.size() == 5);
  CHECK(sink.emits.at(3).operation
        == static_cast<int>(SyncOperation::Delete));
  CHECK(sink.emits.at(3).option == "project_member");
  CHECK(sink.emits.at(3).users == std::vector<int64_t>{42, 7});
  CHECK(sink.emits.at(4).operation
        == static_cast<int>(SyncOperation::Delete));
  CHECK(sink.emits.at(4).option == "project");
  CHECK(sink.emits.at(4).users == std::vector<int64_t>{7});

  ProjectTaskController taskController;

  Json::Value taskBody;
  taskBody["projectId"] = Json::Int64(projectId);
  taskBody["title"] = "Sand the door";
  taskBody["status"] = "todo";
  taskBody["priority"] = "low";
  taskBody["sortOrder"] = 1.0;
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
  REQUIRE(sink.emits.size() == 6);
  CHECK(sink.emits.back().option == "project_task");
  CHECK(sink.emits.back().users == std::vector<int64_t>{42});

  const auto taskMissing =
      drogon::sync_wait(taskController.remove(ownerRequest(), 999));
  CHECK(body(taskMissing)["status"].asInt() == 404);
  CHECK(body(taskMissing)["errors"]["message"] == "Task not found");

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

  Json::Value orphanTaskBody;
  orphanTaskBody["projectId"] = Json::Int64(999);
  orphanTaskBody["title"] = "Orphan";
  orphanTaskBody["status"] = "todo";
  orphanTaskBody["priority"] = "none";
  auto orphanReq = drogon::HttpRequest::newHttpJsonRequest(orphanTaskBody);
  setActor({.req = orphanReq, .sub = 42, .role = UserRole::Owner});
  const auto orphan = drogon::sync_wait(taskController.create(orphanReq));
  CHECK(body(orphan)["status"].asInt() == 404);
  CHECK(body(orphan)["errors"]["message"] == "Project not found");

  CalendarEventController eventController;

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
      drogon::sync_wait(shareController.create(shareReq(42)));
  CHECK(body(eventSelfShare)["status"].asInt() == 409);
  const auto eventGuardShare =
      drogon::sync_wait(shareController.create(shareReq(9)));
  CHECK(body(eventGuardShare)["status"].asInt() == 403);
  CHECK(body(eventGuardShare)["errors"]["message"]
        == "That user cannot see calendar events");

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

  Json::Value shareEditBody;
  shareEditBody["access"] = "edit";
  auto shareEditReq = drogon::HttpRequest::newHttpJsonRequest(shareEditBody);
  setActor({.req = shareEditReq, .sub = 42, .role = UserRole::Owner});
  const auto shareEdited =
      drogon::sync_wait(shareController.update(shareEditReq, shareId));
  CHECK(body(shareEdited)["status"].asInt() == 200);
  CHECK(body(shareEdited)["info"]["access"] == "edit");
  REQUIRE(sink.audits.size() == 4);
  CHECK(sink.audits.back().recordId == shareId);
  CHECK(sink.audits.back().tableName == "calendar_event_share");
  CHECK(sink.audits.back().users == std::vector<int64_t>{42, 7});

  const auto shareGone =
      drogon::sync_wait(shareController.remove(ownerRequest(), shareId));
  CHECK(body(shareGone)["status"].asInt() == 200);
  const auto shareGoneTwice =
      drogon::sync_wait(shareController.remove(ownerRequest(), shareId));
  CHECK(body(shareGoneTwice)["status"].asInt() == 404);
  CHECK(body(shareGoneTwice)["errors"]["message"] == "Share not found");

  const auto eventGone =
      drogon::sync_wait(eventController.remove(ownerRequest(), eventId));
  CHECK(body(eventGone)["status"].asInt() == 200);
  CHECK(body(eventGone)["info"]["deleted"].asBool());
  const auto eventGoneTwice =
      drogon::sync_wait(eventController.remove(ownerRequest(), eventId));
  CHECK(body(eventGoneTwice)["status"].asInt() == 404);
  CHECK(body(eventGoneTwice)["errors"]["message"]
        == "Calendar event not found");

  const auto projectGone =
      drogon::sync_wait(projectController.remove(ownerRequest(), projectId));
  CHECK(body(projectGone)["status"].asInt() == 200);
  CHECK(body(projectGone)["info"]["deleted"].asBool());

  user_change::setProductivitySink(nullptr);
  DbService::setProductivityClient(nullptr);

  drogon::app().quit();
  runner.join();

  std::remove(kIdentityDb);
  std::remove(kProductivityDb);
}
