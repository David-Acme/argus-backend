#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/device-filter.hxx>
#include <config/config-service.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-access.hxx>
#include <auth/role-filter.hxx>
#include <drogon/drogon.h>
#include <errors/validation-exception.hxx>
#include <feature/activity/controllers/activity-controller.hxx>
#include <feature/activity/services/activity-feature-service.hxx>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-action-event.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef ARGUS_SYNC_SCHEMA_PATH
#error "ARGUS_SYNC_SCHEMA_PATH must point at the sync schema.sql"
#endif

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

private:
  std::string path_;
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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty() || rows.front()[0].isNull())
    return {};
  return rows.front()[0].as<std::string>();
}

struct SeedRow
{
  int64_t userId{0};
  const char* table{""};
  const char* module{""};
  const char* action{"update"};
  int64_t createdAt{0};
};

void seed(const SeedRow& row)
{
  DbService::client()->execSqlSync(
      "INSERT INTO user_action_log (user_id, record_id, table_name, module, "
      "action, created_at) VALUES (?, 1, ?, ?, ?, ?)",
      row.userId, std::string(row.table), std::string(row.module),
      std::string(row.action), row.createdAt);
}

drogon::HttpRequestPtr listRequest(
    const std::vector<std::pair<std::string, std::string>>& parameters)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  for (const auto& [name, value] : parameters)
    req->setParameter(name, value);
  return req;
}

Json::Value infoOf(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  if (!json)
    return {};
  return (*json)["info"];
}

Json::Value fetch(ActivityController& controller,
                  const std::vector<std::pair<std::string, std::string>>& parameters)
{
  return infoOf(drogon::sync_wait(controller.list(listRequest(parameters))));
}

std::vector<int64_t> idsOf(const Json::Value& page)
{
  std::vector<int64_t> ids;
  for (const auto& item : page["items"])
    ids.push_back(item["id"].asInt64());
  return ids;
}

bool refused(ActivityController& controller,
             const std::vector<std::pair<std::string, std::string>>& parameters)
{
  try {
    drogon::sync_wait(controller.list(listRequest(parameters)));
  }
  catch (const ValidationException&) {
    return true;
  }
  return false;
}

std::string planOf(const ActivityListInput& input)
{
  std::string sql = "EXPLAIN QUERY PLAN " + UserActionLogRepository::activityStatement(input);
  for (auto at = sql.find('?'); at != std::string::npos; at = sql.find('?', at + 3))
    sql.replace(at, 1, "'1'");
  const auto rows = DbService::client()->execSqlSync(sql);
  std::string plan;
  for (const auto& row : rows)
    plan += row["detail"].as<std::string>() + "\n";
  return plan;
}

void legacyRowsGetTheirModule()
{
  DbService::client()->execSqlSync(
      "CREATE TABLE user_action_log ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL, record_id INTEGER NOT NULL, "
      "table_name TEXT NOT NULL, "
      "action TEXT NOT NULL CHECK (action IN "
      "('create', 'read', 'update', 'delete')), "
      "old_data TEXT NOT NULL DEFAULT '{}', "
      "new_data TEXT NOT NULL DEFAULT '{}', "
      "ip_address TEXT NOT NULL DEFAULT '', "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  for (const char* table :
       {"camera", "zone", "camera_stream", "calendar_event", "project_task", "user", "reminder", "notification"})
    DbService::client()->execSqlSync(
        "INSERT INTO user_action_log (user_id, record_id, table_name, action) "
        "VALUES (1, 1, ?, 'update')",
        std::string(table));

  const AuditFanOut fanOut;
  REQUIRE(fanOut.migrateLegacySchema());
  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));
  REQUIRE(fanOut.backfillActivityModules());

  CHECK(scalar("SELECT COUNT(*) FROM pragma_table_info('user_action_log') WHERE name = 'module'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE module = ''") == "0");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'camera'") == "surveillance");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'zone'") == "surveillance");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'camera_stream'") == "surveillance");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'calendar_event'") == "productivity");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'project_task'") == "productivity");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'user'") == "core");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'reminder'") == "core");
  CHECK(scalar("SELECT module FROM user_action_log WHERE table_name = 'notification'") == "core");

  REQUIRE(fanOut.migrateLegacySchema());
  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));
  REQUIRE(fanOut.backfillActivityModules());
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log") == "8");
  DbService::client()->execSqlSync("DELETE FROM user_action_log");
}

drogon::Task<ActionJournalOutcome> journal(AuditFanOut& fanOut,
                                           const UserActionEvent& event,
                                           const std::string& msgId)
{
  co_return co_await fanOut.handleActionJournal(event.toJson(), msgId);
}

void journaledEventsCarryTheirModule()
{
  AuditFanOut fanOut;

  UserActionEvent camera;
  camera.userId = 2;
  camera.recordId = 9;
  camera.tableName = TableName::Camera;
  camera.action = UserAction::Update;
  REQUIRE(drogon::sync_wait(journal(fanOut, camera, "identity:1")) ==
          ActionJournalOutcome::Acknowledged);
  CHECK(scalar("SELECT module FROM user_action_log WHERE msg_id = 'identity:1'") == "surveillance");
  CHECK(scalar("SELECT table_name FROM user_action_log WHERE msg_id = 'identity:1'") == "camera");

  UserActionEvent event;
  event.userId = 2;
  event.recordId = 4;
  event.tableName = TableName::User;
  event.action = UserAction::Update;
  REQUIRE(drogon::sync_wait(journal(fanOut, event, "identity:2")) ==
          ActionJournalOutcome::Acknowledged);
  CHECK(scalar("SELECT module FROM user_action_log WHERE msg_id = 'identity:2'") == "core");

  UserActionEvent moduleAction;
  moduleAction.userId = 1;
  moduleAction.recordId = 0;
  moduleAction.subject = "module";
  moduleAction.module = "productivity";
  moduleAction.action = UserAction::Update;
  moduleAction.oldData["lifecycle"] = "active";
  moduleAction.newData["lifecycle"] = "disabled";
  REQUIRE(drogon::sync_wait(journal(fanOut, moduleAction, "settings:7")) ==
          ActionJournalOutcome::Acknowledged);
  CHECK(scalar("SELECT table_name FROM user_action_log WHERE msg_id = 'settings:7'") == "module");
  CHECK(scalar("SELECT module FROM user_action_log WHERE msg_id = 'settings:7'") == "productivity");
  CHECK(scalar("SELECT json_extract(new_data, '$.lifecycle') FROM user_action_log WHERE msg_id = 'settings:7'") ==
        "disabled");

  CHECK(drogon::sync_wait(journal(fanOut, moduleAction, "settings:7")) ==
        ActionJournalOutcome::Duplicate);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE msg_id = 'settings:7'") == "1");

  Json::Value unknown = moduleAction.toJson();
  unknown["table_name"] = "no_such_table";
  CHECK(drogon::sync_wait(fanOut.handleActionJournal(unknown, "settings:8")) ==
        ActionJournalOutcome::Refused);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE msg_id = 'settings:8'") == "0");

  UserActionEvent reminder;
  reminder.userId = 3;
  reminder.recordId = 12;
  reminder.tableName = TableName::Reminder;
  reminder.action = UserAction::Create;
  reminder.oldData["title"] = "antes";
  reminder.newData["title"] = "Llamar al doctor";
  reminder.newData["description"] = "resultado de la biopsia";
  REQUIRE(drogon::sync_wait(journal(fanOut, reminder, "productivity:1")) ==
          ActionJournalOutcome::Acknowledged);
  CHECK(scalar("SELECT new_data FROM user_action_log WHERE msg_id = 'productivity:1'") == "{}");
  CHECK(scalar("SELECT old_data FROM user_action_log WHERE msg_id = 'productivity:1'") == "{}");
  CHECK(scalar("SELECT module FROM user_action_log WHERE msg_id = 'productivity:1'") == "core");

  UserActionEvent project;
  project.userId = 3;
  project.recordId = 5;
  project.tableName = TableName::Project;
  project.action = UserAction::Update;
  project.oldData["title"] = "Casa";
  project.newData["title"] = "Casa nueva";
  project.newData["description"] = "presupuesto 40k";
  REQUIRE(drogon::sync_wait(journal(fanOut, project, "productivity:2")) ==
          ActionJournalOutcome::Acknowledged);
  CHECK(scalar("SELECT new_data FROM user_action_log WHERE msg_id = 'productivity:2'") ==
        R"({"title":"Casa nueva"})");
  CHECK(scalar("SELECT old_data FROM user_action_log WHERE msg_id = 'productivity:2'") == "{}");

  DbService::client()->execSqlSync("DELETE FROM user_action_log");
}

class RecordingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len,
            const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg, const drogon::WebSocketMessageType) override
  {
    messages.emplace_back(msg);
  }
  void sendJson(const Json::Value& json,
                const drogon::WebSocketMessageType) override
  {
    messages.push_back(json_util::toString(json));
  }
  [[nodiscard]] const trantor::InetAddress& localAddr() const override
  {
    return addr_;
  }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override
  {
    return addr_;
  }
  [[nodiscard]] bool connected() const override { return closeReason.empty(); }
  [[nodiscard]] bool disconnected() const override
  {
    return !closeReason.empty();
  }
  void shutdown(const drogon::CloseCode, const std::string& reason) override
  {
    closeReason = reason;
  }
  void forceClose() override { closeReason = "forced"; }
  void setPingMessage(const std::string&,
                      const std::chrono::duration<double>&) override
  {
  }
  void disablePing() override {}

  std::vector<std::string> messages;
  std::string closeReason;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

bool runOnLoop(const std::function<void()>& action)
{
  auto* loop = drogon::app().getIOLoop(0);
  if (loop == nullptr)
    return false;
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  loop->runInLoop([&action, done]() {
    action();
    done->set_value();
  });
  return finished.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
}

bool settlesOnLoop(const std::function<bool()>& settled)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    bool answered = false;
    if (!runOnLoop([&settled, &answered]() { answered = settled(); }))
      return false;
    if (answered)
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

int activityFrames(const std::vector<std::string>& messages, std::string& scope)
{
  int count = 0;
  scope.clear();
  for (const auto& message : messages) {
    const Json::Value frame = json_util::fromString(message);
    if (frame["operation"].asInt() !=
        static_cast<int>(SyncOperation::ResourceChanged))
      continue;
    ++count;
    scope = frame["info"]["scope"].asString();
  }
  return count;
}

void anInsertedActionReachesTheActivityScreen()
{
  const auto conn = std::make_shared<RecordingConnection>();
  const drogon::WebSocketConnectionPtr handle = conn;
  REQUIRE(runOnLoop([&handle]() {
    RoomManager{}.join(roleRoom(UserRole::Owner), handle);
  }));

  AuditFanOut fanOut;
  UserActionEvent event;
  event.userId = 4;
  event.recordId = 12;
  event.tableName = TableName::Reminder;
  event.action = UserAction::Update;
  const Json::Value json = event.toJson();
  const std::string firstId = "activity-notice:1";

  CHECK(drogon::sync_wait(sync_fan_out::handleActionPayload(
            {.json = json, .msgId = firstId, .auditFanOut = fanOut})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE msg_id = "
               "'activity-notice:1'") == "1");

  std::string scope;
  REQUIRE(settlesOnLoop([&]() { return activityFrames(conn->messages, scope) == 1; }));
  CHECK(scope == "entries");

  const std::string replayId = "activity-notice:1";
  CHECK(drogon::sync_wait(sync_fan_out::handleActionPayload(
            {.json = json, .msgId = replayId, .auditFanOut = fanOut})) ==
        DurableDisposition::Ack);
  CHECK(scalar("SELECT COUNT(*) FROM user_action_log WHERE msg_id = "
               "'activity-notice:1'") == "1");

  bool stayedOne = false;
  REQUIRE(settlesOnLoop([&]() {
    stayedOne = activityFrames(conn->messages, scope) == 1;
    return stayedOne;
  }));
  CHECK(stayedOne);
  CHECK(scope == "entries");
  CHECK(activityFrames(conn->messages, scope) == 1);

  REQUIRE(runOnLoop([&handle]() { RoomManager{}.leaveAll(handle); }));
  DbService::client()->execSqlSync("DELETE FROM user_action_log");
}

void filtersNarrowTheHistory(ActivityController& controller)
{
  seed({.userId = 1, .table = "module", .module = "surveillance", .action = "update", .createdAt = 1000});
  seed({.userId = 1, .table = "module", .module = "productivity", .action = "update", .createdAt = 1100});
  seed({.userId = 2, .table = "camera", .module = "surveillance", .action = "create", .createdAt = 1200});
  seed({.userId = 3, .table = "calendar_event", .module = "productivity", .action = "delete", .createdAt = 1300});
  seed({.userId = 2, .table = "user", .module = "core", .action = "read", .createdAt = 1400});

  CHECK(idsOf(fetch(controller, {})).size() == 5);
  CHECK(idsOf(fetch(controller, {{"module", "surveillance"}})).size() == 2);
  CHECK(idsOf(fetch(controller, {{"module", "productivity"}})).size() == 2);
  CHECK(idsOf(fetch(controller, {{"module", "core"}})).size() == 1);
  CHECK(idsOf(fetch(controller, {{"module", "agronomy"}})).empty());
  CHECK(idsOf(fetch(controller, {{"userId", "2"}})).size() == 2);
  CHECK(idsOf(fetch(controller, {{"action", "update"}})).size() == 2);
  CHECK(idsOf(fetch(controller, {{"table", "module"}})).size() == 2);
  CHECK(idsOf(fetch(controller, {{"table", "camera"}})).size() == 1);
  CHECK(idsOf(fetch(controller, {{"from", "1100"}, {"to", "1300"}})).size() == 3);
  CHECK(idsOf(fetch(controller, {{"from", "1101"}, {"to", "1299"}})).size() == 1);
  CHECK(idsOf(fetch(controller, {{"module", "surveillance"}, {"userId", "2"}, {"action", "create"}})).size() == 1);
  CHECK(idsOf(fetch(controller, {{"module", "surveillance"}, {"userId", "3"}})).empty());

  const Json::Value page = fetch(controller, {{"module", "surveillance"}});
  REQUIRE(page["items"].size() == 2);
  const Json::Value& newest = page["items"][0];
  CHECK(newest["table"].asString() == "camera");
  CHECK(newest["module"].asString() == "surveillance");
  CHECK(newest["action"].asString() == "create");
  CHECK(newest["userId"].asInt64() == 2);
  CHECK(newest["createdAt"].asInt64() == 1200);
  CHECK(newest.isMember("oldData"));
  CHECK(newest.isMember("newData"));
  CHECK(newest.isMember("recordId"));
  CHECK(newest.isMember("ipAddress"));
  CHECK(page["nextCursor"].isNull());

  DbService::client()->execSqlSync("DELETE FROM user_action_log");
}

void keysetPagingWalksEveryRowOnce(ActivityController& controller)
{
  for (int i = 0; i < 11; ++i)
    seed({.userId = 1, .table = "user", .module = "core", .action = "read", .createdAt = 5000 + (i / 3)});

  std::vector<int64_t> walked;
  std::string cursor;
  int pages = 0;
  while (pages < 10) {
    std::vector<std::pair<std::string, std::string>> parameters{{"limit", "4"}};
    if (!cursor.empty())
      parameters.emplace_back("cursor", cursor);
    const Json::Value page = fetch(controller, parameters);
    for (const int64_t id : idsOf(page))
      walked.push_back(id);
    ++pages;
    if (page["nextCursor"].isNull())
      break;
    cursor = page["nextCursor"].asString();
  }
  CHECK(pages == 3);
  REQUIRE(walked.size() == 11);
  CHECK(std::set<int64_t>(walked.begin(), walked.end()).size() == 11);

  const auto rows = DbService::client()->execSqlSync(
      "SELECT id FROM user_action_log ORDER BY created_at DESC, id DESC");
  std::vector<int64_t> expected;
  for (const auto& row : rows)
    expected.push_back(row["id"].as<int64_t>());
  CHECK(walked == expected);

  const Json::Value exact = fetch(controller, {{"limit", "11"}});
  CHECK(exact["items"].size() == 11);
  CHECK(exact["nextCursor"].isNull());
  const Json::Value clamped = fetch(controller, {{"limit", "100000"}});
  CHECK(clamped["items"].size() == 11);

  DbService::client()->execSqlSync("DELETE FROM user_action_log");
}

void badQueriesAreRefused(ActivityController& controller)
{
  CHECK(refused(controller, {{"action", "explode"}}));
  CHECK(refused(controller, {{"module", "Surveillance"}}));
  CHECK(refused(controller, {{"module", "a b"}}));
  CHECK(refused(controller, {{"table", "users;--"}}));
  CHECK(refused(controller, {{"userId", "abc"}}));
  CHECK(refused(controller, {{"userId", "0"}}));
  CHECK(refused(controller, {{"from", "-5"}}));
  CHECK(refused(controller, {{"from", "2000"}, {"to", "1000"}}));
  CHECK(refused(controller, {{"limit", "0"}}));
  CHECK(refused(controller, {{"limit", "many"}}));
  CHECK(refused(controller, {{"cursor", "nonsense"}}));
  CHECK(refused(controller, {{"cursor", "12"}}));
  CHECK(refused(controller, {{"cursor", "12-"}}));
  CHECK_FALSE(refused(controller, {{"cursor", "12-3"}}));
}

void everyFilterIsServedByAnIndex()
{
  const auto planWith = [](const ActivityFilter& filter) {
    return planOf({.filter = filter, .after = std::nullopt, .limit = 51});
  };

  const std::string module = planWith({.module = "surveillance"});
  CHECK(module.find("idx_user_action_log_module_created") != std::string::npos);
  CHECK(module.find("USE TEMP B-TREE") == std::string::npos);

  const std::string user = planWith({.userId = 7});
  CHECK(user.find("idx_user_action_log_user_created") != std::string::npos);
  CHECK(user.find("USE TEMP B-TREE") == std::string::npos);

  const std::string table = planWith({.table = "module"});
  CHECK(table.find("idx_user_action_log_table_created") != std::string::npos);
  CHECK(table.find("USE TEMP B-TREE") == std::string::npos);

  const std::string range = planWith({.from = 100, .to = 200});
  CHECK(range.find("idx_user_action_log_created") != std::string::npos);
  CHECK(range.find("USE TEMP B-TREE") == std::string::npos);

  const std::string everything = planWith({});
  CHECK(everything.find("idx_user_action_log_created") != std::string::npos);
  CHECK(everything.find("USE TEMP B-TREE") == std::string::npos);
}

void onlyTheOwnerMayReadTheActivity()
{
  const auto allowed = [](UserRole role) {
    return role_access::hasHttpAccess(
        {.role = role, .path = "/sync/activity", .method = drogon::Get});
  };
  CHECK(allowed(UserRole::Owner));
  CHECK_FALSE(allowed(UserRole::Resident));
  CHECK_FALSE(allowed(UserRole::Guard));
  CHECK_FALSE(allowed(UserRole::Guest));
}
}

TEST_CASE("the activity history is journaled with its module and read with filters and keyset paging")
{
  const TempDb db("sync-activity-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = db.path(), .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  legacyRowsGetTheirModule();
  journaledEventsCarryTheirModule();
  anInsertedActionReachesTheActivityScreen();

  ConfigService::setRuntimeString("auth.target", "127.0.0.1:1");
  ConfigService::setRuntimeString("jwt.secret", "sync-activity-test-secret-0123456789abcdef");
  ConfigService::setRuntimeString("jwt.refresh_secret", "sync-activity-test-refresh-0123456789abc");
  const auto device = std::make_shared<DeviceFilter>();
  const auto jwt = std::make_shared<JwtFilter>();
  const auto role = std::make_shared<RoleFilter>();
  ActivityController controller;
  filtersNarrowTheHistory(controller);
  keysetPagingWalksEveryRowOnce(controller);
  badQueriesAreRefused(controller);
  everyFilterIsServedByAnIndex();
  onlyTheOwnerMayReadTheActivity();
}
