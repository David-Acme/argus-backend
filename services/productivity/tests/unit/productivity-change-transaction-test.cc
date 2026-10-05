#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/calendar-event-share/services/calendar-event-share-feature-service.hxx>
#include <feature/calendar-event/services/calendar-event-feature-service.hxx>
#include <feature/project-member/services/project-member-feature-service.hxx>
#include <feature/project-task/services/project-task-feature-service.hxx>
#include <feature/project/services/project-feature-service.hxx>
#include <shared/services/change-sink/nats-productivity-change-sink.hxx>
#include <outbox/outbox-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kTransactionDb = "productivity-change-transaction-test.db";
constexpr int64_t kOwner = 11;
constexpr int64_t kMember = 12;
constexpr int64_t kSharee = 13;

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

class RefusingSink final : public UserChangeSink
{
public:
  explicit RefusingSink(const drogon::orm::DbClient* pooled) : pooled_(pooled)
  {
  }

  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the emit");
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override
  {
    note(input.client);
    if (refuse_)
      throw std::runtime_error("the change sink refused the audit");
    co_return;
  }

  void refuse(bool value) const { refuse_ = value; }
  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool sawClient() const { return sawClient_; }
  [[nodiscard]] bool transactional() const { return transactional_; }

private:
  void note(const drogon::orm::DbClient* client) const
  {
    ++calls_;
    sawClient_ = client != nullptr;
    transactional_ = sawClient_ && client != pooled_;
  }

  mutable bool refuse_{false};
  mutable int calls_{0};
  mutable bool sawClient_{false};
  mutable bool transactional_{false};
  const drogon::orm::DbClient* pooled_;
};

CreateProjectDto projectBody(const std::string& name)
{
  CreateProjectDto dto;
  dto.name = name;
  dto.description = "argus";
  dto.status = "active";
  dto.color = "blue";
  return dto;
}

CreateCalendarEventDto eventBody(const std::string& title)
{
  CreateCalendarEventDto dto;
  dto.title = title;
  dto.description = "argus";
  dto.location = "home";
  dto.color = "blue";
  dto.startsAt = 1700000000;
  return dto;
}

CreateProjectTaskDto taskBody(int64_t projectId, const std::string& title)
{
  CreateProjectTaskDto dto;
  dto.projectId = projectId;
  dto.title = title;
  dto.status = "todo";
  dto.priority = "none";
  return dto;
}

int64_t liveProjects(const std::string& name)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM project WHERE name = ? "
      "AND deleted_at IS NULL",
      name);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

int64_t projectIdByName(const std::string& name)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT id FROM project WHERE name = ? AND deleted_at IS NULL", name);
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

int64_t liveProject(int64_t id)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM project WHERE id = ? "
      "AND deleted_at IS NULL",
      id);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

std::string projectName(int64_t id)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT name FROM project WHERE id = ?", id);
  return rows.empty() ? std::string{} : rows.front()["name"].as<std::string>();
}

int64_t liveTasks(const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM project_task WHERE title = ? "
      "AND deleted_at IS NULL",
      title);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

int64_t liveEvents(const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM calendar_event WHERE title = ? "
      "AND deleted_at IS NULL",
      title);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

int64_t taskIdByTitle(const std::string& title)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT id FROM project_task WHERE title = ? AND deleted_at IS NULL",
      title);
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

std::string eventTitle(int64_t id)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT title FROM calendar_event WHERE id = ?", id);
  return rows.empty() ? std::string{} : rows.front()["title"].as<std::string>();
}

int64_t liveMember(int64_t id)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM project_member WHERE id = ? "
      "AND deleted_at IS NULL",
      id);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}

std::string memberAccess(int64_t id)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT access FROM project_member WHERE id = ? AND deleted_at IS NULL",
      id);
  return rows.empty() ? std::string{} : rows.front()["access"].as<std::string>();
}

int64_t insertMember(int64_t projectId, int64_t userId)
{
  DbService::productivityClient()->execSqlSync(
      "INSERT INTO project_member (project_id, user_id, access) "
      "VALUES (?, ?, 'view')",
      projectId, userId);
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT MAX(id) AS id FROM project_member");
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

int64_t insertShare(int64_t eventId, int64_t userId)
{
  DbService::productivityClient()->execSqlSync(
      "INSERT INTO calendar_event_share (calendar_event_id, user_id, access) "
      "VALUES (?, ?, 'view')",
      eventId, userId);
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT MAX(id) AS id FROM calendar_event_share");
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

int64_t liveShares(int64_t eventId)
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM calendar_event_share "
      "WHERE calendar_event_id = ? AND deleted_at IS NULL",
      eventId);
  return rows.empty() ? -1 : rows.front()["total"].as<int64_t>();
}
}

TEST_CASE("a productivity write and its change are one unit of work")
{
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kTransactionDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));
  DbService::productivityClient()->execSqlSync("PRAGMA foreign_keys = OFF");

  ProjectFeatureService projects;
  ProjectTaskFeatureService tasks;
  ProjectMemberFeatureService members;
  CalendarEventFeatureService events;
  CalendarEventShareFeatureService shares;
  const auto pooled = DbService::productivityClient();

  RefusingSink sink(pooled.get());
  user_change::setProductivitySink(&sink);

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(projects.create({.body = projectBody("Refused Project"), .ownerId = kOwner, .idempotencyKey = {}})),
      std::runtime_error);
  CHECK(liveProjects("Refused Project") == 0);

  sink.refuse(false);
  const int beforeCreate = sink.calls();
  const auto project =
      drogon::sync_wait(projects.create({.body = projectBody("Kept Project"), .ownerId = kOwner, .idempotencyKey = {}}));
  CHECK(project.id > 0);
  CHECK(sink.calls() == beforeCreate + 1);
  CHECK(sink.sawClient());
  CHECK(sink.transactional());
  CHECK(liveProjects("Kept Project") == 1);

  UpdateProjectDto rename;
  rename.name = "Renamed Project";

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(projects.update(
                      {.id = project.id, .body = rename, .actorId = kOwner})),
                  std::runtime_error);
  CHECK(projectName(project.id) == "Kept Project");

  sink.refuse(false);
  CHECK(drogon::sync_wait(
            projects.update({.id = project.id, .body = rename, .actorId = kOwner}))
            .has_value());
  CHECK(projectName(project.id) == "Renamed Project");
  CHECK(sink.transactional());

  const auto live = drogon::sync_wait(
      projects.create({.body = projectBody("Removed Project"), .ownerId = kOwner, .idempotencyKey = {}}));
  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(projects.remove(live.id, kOwner)),
                  std::runtime_error);
  CHECK(liveProject(live.id) == 1);

  sink.refuse(false);
  CHECK(drogon::sync_wait(projects.remove(live.id, kOwner)));
  CHECK(liveProject(live.id) == 0);

  const int64_t parentId = projectIdByName("Renamed Project");
  REQUIRE(parentId > 0);

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(tasks.create({.body = taskBody(parentId, "Refused Task"), .actorId = kOwner, .idempotencyKey = {}})),
      std::runtime_error);
  CHECK(liveTasks("Refused Task") == 0);

  sink.refuse(false);
  CHECK(drogon::sync_wait(tasks.create({.body = taskBody(parentId, "Kept Task"), .actorId = kOwner, .idempotencyKey = {}}))
            .has_value());
  CHECK(liveTasks("Kept Task") == 1);
  CHECK(sink.transactional());

  const int64_t taskId = taskIdByTitle("Kept Task");
  REQUIRE(taskId > 0);

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(tasks.remove(taskId, kOwner)),
                  std::runtime_error);
  CHECK(liveTasks("Kept Task") == 1);

  sink.refuse(false);
  CHECK(drogon::sync_wait(tasks.remove(taskId, kOwner)));
  CHECK(liveTasks("Kept Task") == 0);

  const int64_t memberId = insertMember(parentId, kMember);
  REQUIRE(memberId > 0);
  UpdateProjectMemberDto edit;
  edit.access = "edit";

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(members.update(
                      {.id = memberId, .body = edit, .actorId = kOwner})),
                  std::runtime_error);
  CHECK(memberAccess(memberId) == "view");

  sink.refuse(false);
  const auto membership = drogon::sync_wait(
      members.update({.id = memberId, .body = edit, .actorId = kOwner}));
  REQUIRE(membership.row.has_value());
  CHECK(memberAccess(memberId) == "edit");
  CHECK(sink.transactional());

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(members.remove(memberId, kOwner)),
                  std::runtime_error);
  CHECK(liveMember(memberId) == 1);

  sink.refuse(false);
  CHECK(drogon::sync_wait(members.remove(memberId, kOwner)));
  CHECK(liveMember(memberId) == 0);

  sink.refuse(true);
  CHECK_THROWS_AS(
      drogon::sync_wait(events.create(eventBody("Refused Event"),
                                      {.ownerId = kOwner, .actorId = kOwner, .idempotencyKey = {}})),
      std::runtime_error);
  CHECK(liveEvents("Refused Event") == 0);

  sink.refuse(false);
  const auto event = drogon::sync_wait(events.create(
      eventBody("Kept Event"), {.ownerId = kOwner, .actorId = kOwner, .idempotencyKey = {}}));
  CHECK(event.id > 0);
  CHECK(liveEvents("Kept Event") == 1);
  CHECK(sink.transactional());

  UpdateCalendarEventDto retitle;
  retitle.title = "Renamed Event";

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(events.update(
                      {.id = event.id, .body = retitle, .actorId = kOwner})),
                  std::runtime_error);
  CHECK(liveEvents("Kept Event") == 1);
  CHECK(liveEvents("Renamed Event") == 0);

  sink.refuse(false);
  CHECK(drogon::sync_wait(
            events.update({.id = event.id, .body = retitle, .actorId = kOwner}))
            .has_value());
  CHECK(eventTitle(event.id) == "Renamed Event");
  CHECK(sink.transactional());

  const int64_t shareId = insertShare(event.id, kSharee);
  REQUIRE(shareId > 0);

  sink.refuse(true);
  CHECK_THROWS_AS(drogon::sync_wait(shares.remove(shareId, kOwner)),
                  std::runtime_error);
  CHECK(liveShares(event.id) == 1);

  sink.refuse(false);
  CHECK(drogon::sync_wait(shares.remove(shareId, kOwner)));
  CHECK(liveShares(event.id) == 0);

  NatsProductivityChangeSink durableSink(
      nullptr, NatsProductivityChangeSink::Config{
                   .retryMs = 20, .publishSubject = {}, .streamName = {}});
  user_change::setProductivitySink(&durableSink);

  const auto outbox = NatsProductivityChangeSink::repository();
  const auto durable = drogon::sync_wait(
      projects.create({.body = projectBody("Durable Project"), .ownerId = kOwner, .idempotencyKey = {}}));
  CHECK(durable.id > 0);
  const auto pending = outbox.pendingBatch(10);
  REQUIRE(pending.size() == 1);
  CHECK(pending.front().payload.find(std::to_string(durable.id)) !=
        std::string::npos);

  DbService::productivityClient()->execSqlSync("DROP TABLE change_outbox");

  CHECK_THROWS(drogon::sync_wait(
      projects.create({.body = projectBody("Orphan Project"), .ownerId = kOwner, .idempotencyKey = {}})));
  CHECK(liveProjects("Orphan Project") == 0);
  CHECK(liveProjects("Durable Project") == 1);

  UpdateProjectDto orphan;
  orphan.name = "Orphan Project";
  CHECK_THROWS(drogon::sync_wait(
      projects.update({.id = durable.id, .body = orphan, .actorId = kOwner})));
  CHECK(projectName(durable.id) == "Durable Project");

  CHECK_THROWS(drogon::sync_wait(projects.remove(durable.id, kOwner)));
  CHECK(liveProject(durable.id) == 1);

  user_change::setProductivitySink(nullptr);
  std::remove(kTransactionDb);
  std::remove((std::string(kTransactionDb) + "-wal").c_str());
  std::remove((std::string(kTransactionDb) + "-shm").c_str());
}
