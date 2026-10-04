#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/agenda/services/agenda-announcer.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr int64_t kNow = 1800000000;

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
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

struct SharedBoot
{
  std::string path{"productivity-agenda-test-" + std::to_string(::getpid()) +
                   ".db"};
  std::optional<AppRunner> runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = path,
                                   .name = "default",
                                   .timeout = -1});
    runner.emplace();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!drogon::app().isRunning() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!drogon::app().isRunning())
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA))
      throw std::runtime_error("schema did not apply");
  }

  ~SharedBoot()
  {
    runner.reset();
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
  }
};

void reset()
{
  static SharedBoot boot;
  const auto client = DbService::productivityClient();
  for (const char* table : {"calendar_event_share", "calendar_event", "reminder",
                            "agenda_announcement"})
    client->execSqlSync(std::string("DELETE FROM ") + table);
}

class RecordingNotifier final : public AgendaNotifier
{
public:
  [[nodiscard]] bool send(const AgendaNotice& notice) const override
  {
    const std::scoped_lock lock(mutex);
    sent.push_back(notice);
    return accept;
  }

  std::vector<AgendaNotice> all() const
  {
    const std::scoped_lock lock(mutex);
    return sent;
  }

  mutable std::mutex mutex;
  mutable std::vector<AgendaNotice> sent;
  bool accept{true};
};

struct EventInput
{
  int64_t owner{1};
  std::string title;
  int64_t startsAt{0};
  bool allDay{false};
  bool deleted{false};
};

int64_t insertEvent(const EventInput& input)
{
  const auto client = DbService::productivityClient();
  client->execSqlSync(
      "INSERT INTO calendar_event (owner_id, created_by, title, location, "
      "starts_at, is_all_day, deleted_at) VALUES (?, ?, ?, 'Calle Mayor', ?, ?, ?)",
      input.owner, input.owner, input.title, input.startsAt,
      input.allDay ? 1 : 0,
      input.deleted ? std::optional<int64_t>(kNow) : std::nullopt);
  return client->execSqlSync("SELECT MAX(id) AS id FROM calendar_event")
      .front()["id"]
      .as<int64_t>();
}
}

TEST_CASE("events starting within the lead time are announced once to owner and guests")
{
  reset();
  auto notifier = std::make_shared<RecordingNotifier>();
  auto clock = std::make_shared<std::atomic<int64_t>>(kNow);
  const AgendaAnnouncer announcer(
      {.enabled = true, .leadS = 600, .graceS = 120, .retentionS = 2592000},
      {.notifier = notifier,
       .clock = [clock]() { return clock->load(); },
       .blockingOffLoop = false});

  const int64_t soon = insertEvent({.owner = 1, .title = "Dentista", .startsAt = kNow + 300});
  insertEvent({.owner = 1, .title = "Mañana", .startsAt = kNow + 86400});
  insertEvent({.owner = 1, .title = "Todo el día", .startsAt = kNow + 60, .allDay = true});
  insertEvent({.owner = 1, .title = "Borrado", .startsAt = kNow + 60, .deleted = true});
  insertEvent({.owner = 1, .title = "Pasado", .startsAt = kNow - 600});
  DbService::productivityClient()->execSqlSync(
      "INSERT INTO calendar_event_share (calendar_event_id, user_id) VALUES (?, 2)",
      soon);
  DbService::productivityClient()->execSqlSync(
      "INSERT INTO calendar_event_share (calendar_event_id, user_id, deleted_at) "
      "VALUES (?, 3, ?)",
      soon, kNow);

  const auto report = drogon::sync_wait(announcer.sweep());
  CHECK(report.events == 1);
  const auto sent = notifier->all();
  REQUIRE(sent.size() == 1);
  CHECK(sent.front().userIds == std::vector<int64_t>{1, 2});
  CHECK(sent.front().title == "Dentista");
  CHECK(sent.front().body == agenda_notice::clockTime(kNow + 300) + " · Calle Mayor");
  CHECK(sent.front().data["kind"].asString() == "agenda_event");
  CHECK(sent.front().data["threadKey"].asString() ==
        "agenda:event:" + std::to_string(soon) + ":" + std::to_string(kNow + 300));
  CHECK(sent.front().commandId == sent.front().data["threadKey"].asString());

  clock->fetch_add(30);
  CHECK(drogon::sync_wait(announcer.sweep()).events == 0);
  CHECK(notifier->all().size() == 1);

  DbService::productivityClient()->execSqlSync(
      "UPDATE calendar_event SET starts_at = ? WHERE id = ?", kNow + 500, soon);
  CHECK(drogon::sync_wait(announcer.sweep()).events == 1);
}

TEST_CASE("an undelivered announcement is retried on the next sweep")
{
  reset();
  auto notifier = std::make_shared<RecordingNotifier>();
  notifier->accept = false;
  const AgendaAnnouncer announcer(
      {.enabled = true, .leadS = 600, .graceS = 120, .retentionS = 2592000},
      {.notifier = notifier, .clock = []() { return kNow; }, .blockingOffLoop = false});
  insertEvent({.owner = 4, .title = "Reunión", .startsAt = kNow + 60});
  CHECK(drogon::sync_wait(announcer.sweep()).failed == 1);
  notifier->accept = true;
  CHECK(drogon::sync_wait(announcer.sweep()).events == 1);
  CHECK(drogon::sync_wait(announcer.sweep()).events == 0);
}

TEST_CASE("due reminders are announced to their target, completed ones never")
{
  reset();
  auto notifier = std::make_shared<RecordingNotifier>();
  const AgendaAnnouncer announcer(
      {.enabled = true, .leadS = 600, .graceS = 120, .retentionS = 2592000},
      {.notifier = notifier, .clock = []() { return kNow; }, .blockingOffLoop = false});
  const auto client = DbService::productivityClient();
  client->execSqlSync(
      "INSERT INTO reminder (target_user_id, title, description, scheduled_at) "
      "VALUES (5, 'Sacar la basura', 'Hoy pasa el camión', ?)",
      kNow - 10);
  client->execSqlSync(
      "INSERT INTO reminder (target_user_id, title, scheduled_at, is_completed) "
      "VALUES (5, 'Hecho', ?, 1)",
      kNow - 10);
  client->execSqlSync(
      "INSERT INTO reminder (target_user_id, title, scheduled_at) "
      "VALUES (5, 'Luego', ?)",
      kNow + 60);
  const auto report = drogon::sync_wait(announcer.sweep());
  CHECK(report.reminders == 1);
  const auto sent = notifier->all();
  REQUIRE(sent.size() == 1);
  CHECK(sent.front().userIds == std::vector<int64_t>{5});
  CHECK(sent.front().body == "Hoy pasa el camión");
  CHECK(sent.front().data["kind"].asString() == "agenda_reminder");
}

TEST_CASE("without a notifier or switched off, the announcer does nothing")
{
  reset();
  const AgendaAnnouncer off({.enabled = false, .leadS = 600, .graceS = 120, .retentionS = 1},
                            {.notifier = std::make_shared<RecordingNotifier>(),
                             .clock = {},
                             .blockingOffLoop = false});
  CHECK_FALSE(off.enabled());
  const AgendaAnnouncer unwired({.enabled = true, .leadS = 600, .graceS = 120, .retentionS = 1},
                                {.notifier = nullptr, .clock = {}, .blockingOffLoop = false});
  CHECK_FALSE(unwired.enabled());
  insertEvent({.owner = 1, .title = "X", .startsAt = kNow + 10});
  CHECK(drogon::sync_wait(unwired.sweep()).events == 0);
}
