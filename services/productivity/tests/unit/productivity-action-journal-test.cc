#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <drogon/drogon.h>
#include <feature/calendar-event-share/services/calendar-event-share-feature-service.hxx>
#include <feature/calendar-event/services/calendar-event-feature-service.hxx>
#include <feature/project-member/services/project-member-feature-service.hxx>
#include <feature/project-task/services/project-task-feature-service.hxx>
#include <feature/project/services/project-feature-service.hxx>
#include <feature/reminder/services/reminder-feature-service.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-repository.hxx>
#include <shared/services/change-sink/nats-productivity-change-sink.hxx>
#include <sqlite/db-service.hxx>
#include <sync/user-change-sink.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "productivity-action-journal-test.db";
constexpr int64_t kOwner = 21;
constexpr int64_t kMember = 22;

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

class RecordingSink final : public UserChangeSink
{
public:
  [[nodiscard]] drogon::Task<void> emitUsers(const UserEmitInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    actions_.push_back(input);
    co_return;
  }

  [[nodiscard]] std::vector<ActionPublishInput> take() const
  {
    std::vector<ActionPublishInput> taken = std::move(actions_);
    actions_.clear();
    return taken;
  }

  void drain() const { actions_.clear(); }

private:
  mutable std::vector<ActionPublishInput> actions_;
};

struct Expected
{
  TableName table;
  UserAction action;
  int64_t actor;
  std::optional<std::string> title;
};

void expectOnly(const std::vector<ActionPublishInput>& actions, const Expected& expected)
{
  REQUIRE(actions.size() == 1);
  const auto& published = actions.front();
  const auto& event = published.event;
  CHECK(event.tableName == expected.table);
  CHECK(event.action == expected.action);
  CHECK(event.userId == expected.actor);
  CHECK(event.recordId > 0);
  CHECK(event.oldData.isNull());
  CHECK(event.ipAddress.empty());
  CHECK(published.client != nullptr);
  if (!expected.title) {
    CHECK(event.newData.isNull());
    return;
  }
  REQUIRE(event.newData.isObject());
  CHECK(event.newData.getMemberNames() == std::vector<std::string>{"title"});
  CHECK(event.newData["title"].asString() == *expected.title);
}

CreateProjectDto projectBody(const std::string& name)
{
  CreateProjectDto dto;
  dto.name = name;
  dto.description = "the private description";
  dto.status = "active";
  dto.color = "blue";
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

CreateCalendarEventDto eventBody(const std::string& title)
{
  CreateCalendarEventDto dto;
  dto.title = title;
  dto.description = "the private description";
  dto.location = "home";
  dto.color = "blue";
  dto.startsAt = 1700000000;
  return dto;
}

int64_t insertRow(const std::string& sql, int64_t parentId, int64_t userId)
{
  DbService::productivityClient()->execSqlSync(sql, parentId, userId);
  const auto rows = DbService::productivityClient()->execSqlSync("SELECT last_insert_rowid() AS id");
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}
}

TEST_CASE("every productivity write journals who, kind, table and record, and a title only for a shared row")
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

  const ProjectFeatureService projects;
  const ProjectTaskFeatureService tasks;
  const ProjectMemberFeatureService members;
  const CalendarEventFeatureService events;
  const CalendarEventShareFeatureService shares;
  const ReminderFeatureService reminders;

  RecordingSink sink;
  user_change::setProductivitySink(&sink);

  {
    INFO("a project nobody else sees is journaled without its name");
    const auto personal = drogon::sync_wait(
        projects.create({.body = projectBody("Mi diario"), .ownerId = kOwner, .idempotencyKey = {}}));
    expectOnly(sink.take(), {.table = TableName::Project, .action = UserAction::Create, .actor = kOwner, .title = std::nullopt});

    UpdateProjectDto rename;
    rename.name = "Mi diario íntimo";
    REQUIRE(drogon::sync_wait(projects.update({.id = personal.id, .body = rename, .actorId = kOwner})).has_value());
    expectOnly(sink.take(), {.table = TableName::Project, .action = UserAction::Update, .actor = kOwner, .title = std::nullopt});

    REQUIRE(drogon::sync_wait(tasks.create({.body = taskBody(personal.id, "Escribir"), .actorId = kOwner, .idempotencyKey = {}}))
                .has_value());
    expectOnly(sink.take(), {.table = TableName::ProjectTask, .action = UserAction::Create, .actor = kOwner, .title = std::nullopt});

    CHECK(drogon::sync_wait(projects.remove(personal.id, kOwner)));
    expectOnly(sink.take(), {.table = TableName::Project, .action = UserAction::Delete, .actor = kOwner, .title = std::nullopt});
  }

  {
    INFO("a project with members carries its name, and so do its tasks");
    const auto household = drogon::sync_wait(
        projects.create({.body = projectBody("Mudanza"), .ownerId = kOwner, .idempotencyKey = {}}));
    sink.drain();
    const int64_t memberId = insertRow(
        "INSERT INTO project_member (project_id, user_id, access) VALUES (?, ?, 'edit')", household.id, kMember);
    REQUIRE(memberId > 0);

    UpdateProjectDto rename;
    rename.name = "Mudanza a Lima";
    REQUIRE(drogon::sync_wait(projects.update({.id = household.id, .body = rename, .actorId = kOwner})).has_value());
    expectOnly(sink.take(), {.table = TableName::Project, .action = UserAction::Update, .actor = kOwner, .title = "Mudanza a Lima"});

    const auto task = drogon::sync_wait(
        tasks.create({.body = taskBody(household.id, "Embalar libros"), .actorId = kMember, .idempotencyKey = {}}));
    REQUIRE(task.has_value());
    expectOnly(sink.take(), {.table = TableName::ProjectTask, .action = UserAction::Create, .actor = kMember, .title = "Embalar libros"});

    UpdateProjectTaskDto done;
    done.status = "done";
    REQUIRE(drogon::sync_wait(tasks.update({.id = task.value_or(ProjectTaskSchema{}).id, .body = done, .actorId = kMember}))
                .has_value());
    expectOnly(sink.take(), {.table = TableName::ProjectTask, .action = UserAction::Update, .actor = kMember, .title = "Embalar libros"});

    CHECK(drogon::sync_wait(tasks.remove(task.value_or(ProjectTaskSchema{}).id, kMember)));
    expectOnly(sink.take(), {.table = TableName::ProjectTask, .action = UserAction::Delete, .actor = kMember, .title = "Embalar libros"});

    UpdateProjectMemberDto access;
    access.access = "view";
    REQUIRE(drogon::sync_wait(members.update({.id = memberId, .body = access, .actorId = kOwner})).row.has_value());
    expectOnly(sink.take(), {.table = TableName::ProjectMember, .action = UserAction::Update, .actor = kOwner, .title = std::nullopt});

    CHECK(drogon::sync_wait(projects.remove(household.id, kOwner)));
    expectOnly(sink.take(), {.table = TableName::Project, .action = UserAction::Delete, .actor = kOwner, .title = "Mudanza a Lima"});
  }

  {
    INFO("a member removal journals the membership without any name");
    const auto project = drogon::sync_wait(
        projects.create({.body = projectBody("Jardín"), .ownerId = kOwner, .idempotencyKey = {}}));
    sink.drain();
    const int64_t memberId = insertRow(
        "INSERT INTO project_member (project_id, user_id, access) VALUES (?, ?, 'view')", project.id, kMember);
    CHECK(drogon::sync_wait(members.remove(memberId, kOwner)));
    expectOnly(sink.take(), {.table = TableName::ProjectMember, .action = UserAction::Delete, .actor = kOwner, .title = std::nullopt});
  }

  {
    INFO("a calendar event is titled only while it is shared");
    const auto event = drogon::sync_wait(
        events.create(eventBody("Médico"), {.ownerId = kOwner, .actorId = kOwner, .idempotencyKey = {}}));
    expectOnly(sink.take(), {.table = TableName::CalendarEvent, .action = UserAction::Create, .actor = kOwner, .title = std::nullopt});

    UpdateCalendarEventDto retitle;
    retitle.title = "Médico: resultados";
    REQUIRE(drogon::sync_wait(events.update({.id = event.id, .body = retitle, .actorId = kOwner})).has_value());
    expectOnly(sink.take(), {.table = TableName::CalendarEvent, .action = UserAction::Update, .actor = kOwner, .title = std::nullopt});

    const int64_t shareId = insertRow(
        "INSERT INTO calendar_event_share (calendar_event_id, user_id, access) VALUES (?, ?, 'view')", event.id, kMember);
    UpdateCalendarEventDto party;
    party.title = "Cumpleaños";
    REQUIRE(drogon::sync_wait(events.update({.id = event.id, .body = party, .actorId = kOwner})).has_value());
    expectOnly(sink.take(), {.table = TableName::CalendarEvent, .action = UserAction::Update, .actor = kOwner, .title = "Cumpleaños"});

    UpdateCalendarEventShareDto access;
    access.access = "edit";
    REQUIRE(drogon::sync_wait(shares.update({.id = shareId, .body = access, .actorId = kOwner})).row.has_value());
    expectOnly(sink.take(),
               {.table = TableName::CalendarEventShare, .action = UserAction::Update, .actor = kOwner, .title = std::nullopt});

    CHECK(drogon::sync_wait(shares.remove(shareId, kOwner)));
    expectOnly(sink.take(),
               {.table = TableName::CalendarEventShare, .action = UserAction::Delete, .actor = kOwner, .title = std::nullopt});

    CHECK(drogon::sync_wait(events.remove(event.id, kOwner)));
    expectOnly(sink.take(), {.table = TableName::CalendarEvent, .action = UserAction::Delete, .actor = kOwner, .title = std::nullopt});
  }

  {
    INFO("a reminder never carries its content");
    CreateReminderDto body;
    body.title = "Tomar la pastilla";
    body.description = "la del corazón";
    body.scheduledAt = 1790000000;
    const auto reminder = drogon::sync_wait(reminders.create({.body = body, .userId = kMember, .idempotencyKey = {}}));
    expectOnly(sink.take(), {.table = TableName::Reminder, .action = UserAction::Create, .actor = kMember, .title = std::nullopt});

    UpdateReminderDto complete;
    complete.isCompleted = true;
    REQUIRE(drogon::sync_wait(reminders.update({.id = reminder.id, .body = complete, .userId = kMember})).has_value());
    expectOnly(sink.take(), {.table = TableName::Reminder, .action = UserAction::Update, .actor = kMember, .title = std::nullopt});

    CHECK(drogon::sync_wait(reminders.remove({.id = reminder.id, .userId = kMember})));
    expectOnly(sink.take(), {.table = TableName::Reminder, .action = UserAction::Delete, .actor = kMember, .title = std::nullopt});
  }

  {
    INFO("the durable sink journals on its own subject, with nothing of the row");
    NatsProductivityChangeSink durable(nullptr,
                                       NatsProductivityChangeSink::Config{.retryMs = 20,
                                                                          .publishSubject = {},
                                                                          .streamName = {},
                                                                          .actionSubject = {},
                                                                          .actionStreamName = {}});
    user_change::setProductivitySink(&durable);
    const auto outbox = NatsProductivityChangeSink::repository();
    CreateReminderDto body;
    body.title = "Secreto";
    body.description = "nadie debe leerlo";
    body.scheduledAt = 1790000000;
    drogon::sync_wait(reminders.create({.body = body, .userId = kMember, .idempotencyKey = {}}));
    const auto pending = outbox.pendingBatch(10);
    const auto action = std::ranges::find_if(pending, [](const outbox::OutboxRow& row) {
      return row.subject == nats_subject::kProductivityUserAction;
    });
    REQUIRE(action != pending.end());
    CHECK(action->payload.find("Secreto") == std::string::npos);
    CHECK(action->payload.find("nadie") == std::string::npos);
    CHECK(action->payload.find("\"new_data\":null") != std::string::npos);
    CHECK(action->payload.find("\"old_data\":null") != std::string::npos);
    CHECK(std::ranges::count_if(pending, [](const outbox::OutboxRow& row) {
            return row.subject == nats_subject::kProductivityUserAction;
          }) == 1);
  }

  user_change::setProductivitySink(nullptr);
  removeDatabase();
}
