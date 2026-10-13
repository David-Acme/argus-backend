#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/project-member/services/project-member-feature-service.hxx>
#include <feature/project-task/services/project-task-feature-service.hxx>
#include <feature/project/services/project-feature-service.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "productivity-task-assignee-test.db";
constexpr int64_t kOwner = 31;
constexpr int64_t kMember = 32;
constexpr int64_t kStranger = 33;

using test_support::AppRunner;

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

void removeDatabase()
{
  for (const char* suffix : {"", "-wal", "-shm"})
    std::remove((std::string(kDb) + suffix).c_str());
}

class AssigneeAuditSink final : public UserChangeSink
{
public:
  [[nodiscard]] drogon::Task<void> emitUsers(const UserEmitInput&) const override { co_return; }

  [[nodiscard]] drogon::Task<void> publishAudit(const UserAuditInput& input) const override
  {
    audits_.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishAction(const ActionPublishInput&) const override { co_return; }

  [[nodiscard]] const std::vector<UserAuditInput>& audits() const { return audits_; }

  void clear() const { audits_.clear(); }

private:
  mutable std::vector<UserAuditInput> audits_;
};

CreateProjectDto projectBody()
{
  CreateProjectDto dto;
  dto.name = "Mudanza";
  dto.description = "";
  dto.status = "active";
  dto.color = "blue";
  return dto;
}

CreateProjectTaskDto taskBody(int64_t projectId, std::optional<int64_t> assigneeId)
{
  CreateProjectTaskDto dto;
  dto.projectId = projectId;
  dto.title = "Embalar libros";
  dto.status = "todo";
  dto.priority = "none";
  dto.assigneeId = assigneeId;
  return dto;
}

std::optional<int64_t> assigneeOf(int64_t taskId)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT assignee_id FROM project_task WHERE id = ?", taskId);
  if (rows.empty() || rows.front()["assignee_id"].isNull())
    return std::nullopt;
  return rows.front()["assignee_id"].as<int64_t>();
}

int64_t insertMember(int64_t projectId, int64_t userId)
{
  DbService::productivityClient()->execSqlSync(
      "INSERT INTO project_member (project_id, user_id, access) VALUES (?, ?, 'edit')", projectId, userId);
  const auto rows = DbService::productivityClient()->execSqlSync("SELECT last_insert_rowid() AS id");
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

std::optional<int> statusOfRefusal(const std::function<void()>& run)
{
  try {
    run();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  return std::nullopt;
}
}

TEST_CASE("a task is assigned only to the project's owner or a member, can be unassigned, and loses a member who leaves")
{
  removeDatabase();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot());
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));
  DbService::productivityClient()->execSqlSync("PRAGMA foreign_keys = OFF");

  AssigneeAuditSink sink;
  user_change::setProductivitySink(&sink);
  const ProjectFeatureService projects;
  const ProjectTaskFeatureService tasks;
  const ProjectMemberFeatureService members;

  const auto project =
      drogon::sync_wait(projects.create({.body = projectBody(), .ownerId = kOwner, .idempotencyKey = {}}));
  const int64_t membership = insertMember(project.id, kMember);

  CHECK(statusOfRefusal([&] {
          drogon::sync_wait(tasks.create({.body = taskBody(project.id, kStranger), .actorId = kOwner, .idempotencyKey = {}}));
        }) == std::optional<int>{422});
  CHECK(DbService::productivityClient()->execSqlSync("SELECT id FROM project_task").empty());

  const auto toOwner =
      drogon::sync_wait(tasks.create({.body = taskBody(project.id, kOwner), .actorId = kOwner, .idempotencyKey = {}}));
  REQUIRE(toOwner.has_value());
  CHECK(assigneeOf(toOwner.value_or(ProjectTaskSchema{}).id) == std::optional<int64_t>{kOwner});

  const auto toMember =
      drogon::sync_wait(tasks.create({.body = taskBody(project.id, kMember), .actorId = kOwner, .idempotencyKey = {}}));
  REQUIRE(toMember.has_value());
  const int64_t memberTask = toMember.value_or(ProjectTaskSchema{}).id;

  UpdateProjectTaskDto stranger;
  stranger.assigneeId = kStranger;
  CHECK(statusOfRefusal([&] {
          drogon::sync_wait(tasks.update({.id = memberTask, .body = stranger, .actorId = kOwner}));
        }) == std::optional<int>{422});
  CHECK(assigneeOf(memberTask) == std::optional<int64_t>{kMember});

  UpdateProjectTaskDto clear = UpdateProjectTaskDto::fromJson([] {
    Json::Value json(Json::objectValue);
    json["assigneeId"] = Json::Value(Json::nullValue);
    return json;
  }());
  CHECK(clear.clearsAssignee);
  REQUIRE(drogon::sync_wait(tasks.update({.id = memberTask, .body = clear, .actorId = kOwner})).has_value());
  CHECK_FALSE(assigneeOf(memberTask).has_value());

  UpdateProjectTaskDto back;
  back.assigneeId = kMember;
  REQUIRE(drogon::sync_wait(tasks.update({.id = memberTask, .body = back, .actorId = kOwner})).has_value());
  CHECK(assigneeOf(memberTask) == std::optional<int64_t>{kMember});

  sink.clear();
  CHECK(drogon::sync_wait(members.remove(membership, kOwner)));
  CHECK_FALSE(assigneeOf(memberTask).has_value());
  CHECK(assigneeOf(toOwner.value_or(ProjectTaskSchema{}).id) == std::optional<int64_t>{kOwner});
  REQUIRE(sink.audits().size() == 1);
  const auto& audit = sink.audits().front();
  CHECK(audit.tableName == TableName::ProjectTask);
  CHECK(audit.recordId == memberTask);
  CHECK(audit.before["assigneeId"].asInt64() == kMember);
  CHECK(audit.after["assigneeId"].isNull());
  CHECK(audit.userIds == std::vector<int64_t>{kOwner});

  user_change::setProductivitySink(nullptr);
  removeDatabase();
}
