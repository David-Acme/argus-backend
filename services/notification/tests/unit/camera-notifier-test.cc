#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/camera-notification/repositories/camera-fallback-log/camera-fallback-log-repository.hxx>
#include <feature/camera-notification/services/camera-object-notifier.hxx>
#include <identity/identity-client.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at the notification schema.sql"
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

class SharedBoot
{
public:
  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = db_.path(),
                                   .name = "default",
                                   .timeout = -1});
    runner_.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
  }

  [[nodiscard]] bool applySchema() const
  {
    return DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA);
  }

private:
  TempDb db_{"camera-notifier-test-notification"};
  std::optional<AppRunner> runner_;
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

int64_t countRows(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

int64_t cameraNotificationCount()
{
  return countRows(
      "SELECT COUNT(*) AS total FROM notification WHERE type = 'camera'");
}

bool waitForCameraNotifications(int64_t expected,
                                std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cameraNotificationCount() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cameraNotificationCount() >= expected;
}

std::string fallbackReason(int64_t cameraId, const std::string& rule)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT reason FROM camera_fallback_event WHERE camera_id = ? AND "
      "rule = ? ORDER BY id DESC LIMIT 1",
      cameraId, rule);
  if (rows.empty())
    return {};
  return rows.front()["reason"].as<std::string>();
}

struct AtLocalHourInput
{
  int hour{0};
  int minute{0};
  int day{0};
};

int64_t atLocalHour(const AtLocalHourInput& input)
{
  const int hour = input.hour;
  const int minute = input.minute;
  const int day = input.day;

  std::tm local{};
  local.tm_year = 2025 - 1900;
  local.tm_mon = 5;
  local.tm_mday = day;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = 0;
  const std::time_t tick = std::mktime(&local);
  return static_cast<int64_t>(tick) * 1000;
}

struct EventJsonInput
{
  int64_t cameraId{0};
  const char* rule;
  const char* severity;
  std::string eventId;
};

Json::Value eventJson(const EventJsonInput& input)
{
  const int64_t cameraId = input.cameraId;
  const char* rule = input.rule;
  const char* severity = input.severity;

  Json::Value event;
  event["cameraId"] = Json::Int64(cameraId);
  event["cameraName"] = "Front door";
  event["rule"] = rule;
  event["severity"] = severity;
  if (!input.eventId.empty())
    event["eventId"] = input.eventId;
  Json::Value objects;
  Json::Value object;
  object["class"] = "person";
  object["confidence"] = 0.9;
  objects.append(object);
  event["objects"] = objects;
  return event;
}

class RecordingIdentityClient final : public IdentityClient
{
public:
  explicit RecordingIdentityClient(std::vector<int64_t> userIds)
      : IdentityClient("127.0.0.1:1", "notification-identity"),
        userIds_(std::move(userIds))
  {
  }

  [[nodiscard]] std::optional<std::vector<int64_t>>
  listNotifiableUsers() const override
  {
    return userIds_;
  }

private:
  std::vector<int64_t> userIds_;
};

CameraNotificationPolicy::Config openConfig()
{
  return {.budgetPerHour = 6,
          .silentStartHour = -1,
          .silentEndHour = -1,
          .guardTimeoutMs = 30000};
}

std::shared_ptr<CameraObjectNotifier> makeNotifier(
    const CameraNotificationPolicy::Config& config,
    std::shared_ptr<IdentityClient> identityClient)
{
  return std::make_shared<CameraObjectNotifier>(
      config,
      CameraNotifierDependencies{.identityClient = std::move(identityClient),
                                 .delivery = {}});
}
}

TEST_CASE("the notification budget allows budget_per_hour then suppresses")
{
  CameraNotificationPolicy policy({2, -1, -1, 30000});
  const int64_t start = atLocalHour({.hour = 12, .minute = 0, .day = 15});

  CHECK(policy.shouldNotify(1, start));
  CHECK(policy.shouldNotify(1, start + 1000));
  CHECK_FALSE(policy.shouldNotify(1, start + 2000));

  CHECK(policy.shouldNotify(1, start + 3600000));
  CHECK(policy.shouldNotify(2, start + 1000));
}

TEST_CASE("silent hours suppress and support wrapping")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 23, .minute = 0, .day = 15})));
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 2, .minute = 0, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 12, .minute = 0, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 21, .minute = 59, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 6, .minute = 0, .day = 15})));

  CameraNotificationPolicy disabled({.budgetPerHour = 6,
                                     .silentStartHour = -1,
                                     .silentEndHour = -1,
                                     .guardTimeoutMs = 30000});
  CHECK(disabled.shouldNotify(1, atLocalHour({.hour = 23, .minute = 0, .day = 15})));
}

TEST_CASE("the digest summarizes suppressed events after the window closes")
{
  CameraNotificationPolicy policy({1, -1, -1, 30000});
  const int64_t start = atLocalHour({.hour = 12, .minute = 0, .day = 15});

  CHECK(policy.shouldNotify(1, start));
  policy.countSuppressed(1, "person");
  policy.countSuppressed(1, "person");
  policy.countSuppressed(1, "car");

  CHECK(policy.takeDigest(1, start + 1000).empty());

  const std::string digest = policy.takeDigest(1, start + 3600000);
  CHECK(digest.find("3 events suppressed") != std::string::npos);
  CHECK(digest.find("2 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);

  CHECK(policy.takeDigest(1, start + 7200000).empty());
}

TEST_CASE("a digest flushes when the silent window ends")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  const int64_t night = atLocalHour({.hour = 23, .minute = 0, .day = 15});

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  const std::string digest = policy.takeDigest(1, atLocalHour({.hour = 6, .minute = 0, .day = 16}));
  CHECK(digest.find("1 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
}

TEST_CASE("a pending digest survives the hour-roll race")
{
  CameraNotificationPolicy policy({6, -1, -1});
  const int64_t start = atLocalHour({.hour = 12, .minute = 0, .day = 15});

  CHECK(policy.shouldNotify(1, start));
  policy.countSuppressed(1, "person");
  policy.countSuppressed(1, "car");

  CHECK(policy.shouldNotify(1, start + 3600000));
  const std::string digest = policy.takeDigest(1, start + 3600001);
  CHECK(digest.find("2 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);

  CHECK(policy.takeDigest(1, start + 3600002).empty());
}

TEST_CASE("counts suppressed inside silent hours carry until the window ends")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  const int64_t night = atLocalHour({.hour = 23, .minute = 0, .day = 15});

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 0, .minute = 5, .day = 16})));
  policy.countSuppressed(1, "car");
  CHECK(policy.takeDigest(1, atLocalHour({.hour = 1, .minute = 0, .day = 16})).empty());

  const std::string digest = policy.takeDigest(1, atLocalHour({.hour = 6, .minute = 0, .day = 16}));
  CHECK(digest.find("2 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);
}

TEST_CASE("the guard heartbeat gates the raw fallback window")
{
  CameraNotificationPolicy policy({.budgetPerHour = 6,
                                   .silentStartHour = -1,
                                   .silentEndHour = -1,
                                   .guardTimeoutMs = 30000});
  CHECK_FALSE(policy.guardReady(1000));
  policy.markGuardHeartbeat(1000);
  CHECK(policy.guardReady(1000));
  CHECK(policy.guardReady(31000));
  CHECK_FALSE(policy.guardReady(31001));
}

TEST_CASE("the consumer applies the budget and records camera notifications")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  const int64_t before = cameraNotificationCount();

  auto notifier = makeNotifier(
      openConfig(), std::make_shared<RecordingIdentityClient>(
                        std::vector<int64_t>{1, 2}));

  const auto hardEvent = [](int index) {
    return eventJson({.cameraId = 1,
                      .rule = "person_in_alert_zone",
                      .severity = "critical",
                      .eventId = "1:hard:" + std::to_string(index)});
  };

  notifier->handle(hardEvent(0));
  REQUIRE(waitForCameraNotifications(before + 2, std::chrono::seconds(10)));
  {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT title, body, data FROM notification WHERE type = 'camera' "
        "ORDER BY id DESC LIMIT 1");
    REQUIRE(rows.size() == 1);
    CHECK(rows.front()["title"].as<std::string>() ==
          "Front door: person_in_alert_zone");
    CHECK(rows.front()["body"].as<std::string>() ==
          "Severity critical; detected person");
    CHECK(json_util::fromString(rows.front()["data"].as<std::string>())
              ["cameraId"].asInt64() == 1);
  }

  for (int i = 1; i < 6; ++i)
    notifier->handle(hardEvent(i));
  REQUIRE(waitForCameraNotifications(before + 12, std::chrono::seconds(10)));
  notifier->handle(hardEvent(6));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before + 12);

  notifier->handle(eventJson({.cameraId = 1,
                              .rule = "person_day",
                              .severity = "info",
                              .eventId = "1:soft:1"}));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before + 12);

  notifier->handle(json_util::fromString("[1, 2, 3]"));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before + 12);
}

TEST_CASE("a redelivered camera event records nothing a second time")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  const int64_t before = cameraNotificationCount();

  auto notifier = makeNotifier(
      openConfig(), std::make_shared<RecordingIdentityClient>(
                        std::vector<int64_t>{1, 2}));
  const auto event = eventJson({.cameraId = 42,
                                .rule = "person_in_alert_zone",
                                .severity = "critical",
                                .eventId = "42:1700000000000:7"});

  notifier->handle(event);
  REQUIRE(waitForCameraNotifications(before + 2, std::chrono::seconds(10)));
  notifier->handle(event);
  notifier->handle(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before + 2);
}

TEST_CASE("delivery needs the identity roster and stops short without it")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  const int64_t before = cameraNotificationCount();
  const auto event = eventJson({.cameraId = 8,
                                .rule = "person_in_alert_zone",
                                .severity = "critical",
                                .eventId = {}});

  auto withoutIdentity = makeNotifier(openConfig(), nullptr);
  withoutIdentity->handle(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before);

  auto withEmptyRoster = makeNotifier(
      openConfig(), std::make_shared<RecordingIdentityClient>(
                        std::vector<int64_t>{}));
  withEmptyRoster->handle(event);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(cameraNotificationCount() == before);

  auto withRoster = makeNotifier(
      openConfig(), std::make_shared<RecordingIdentityClient>(
                        std::vector<int64_t>{4}));
  withRoster->handle(event);
  REQUIRE(waitForCameraNotifications(before + 1, std::chrono::seconds(10)));
  CHECK(cameraNotificationCount() == before + 1);
}

namespace
{
struct FallbackEventInput
{
  int64_t cameraId{0};
  std::string identityState;
  double scoreMedian{0.0};
  int scoreSamples{0};
  int64_t dwellMs{0};
  bool withHistory{true};
};

Json::Value fallbackEvent(const FallbackEventInput& input)
{
  Json::Value event;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "Front door";
  event["rule"] = "person_in_alert_zone";
  event["severity"] = "critical";
  Json::Value object;
  object["class"] = "person";
  object["confidence"] = 0.9;
  if (!input.identityState.empty())
    object["identityState"] = input.identityState;
  if (input.withHistory) {
    object["scoreMedian"] = input.scoreMedian;
    object["scoreSamples"] = input.scoreSamples;
    object["dwellMs"] = Json::Int64(input.dwellMs);
  }
  Json::Value objects;
  objects.append(object);
  event["objects"] = objects;
  return event;
}

CameraNotificationPolicy::Config fallbackConfig()
{
  return {.budgetPerHour = 6,
          .silentStartHour = -1,
          .silentEndHour = -1,
          .guardTimeoutMs = 30000,
          .fallbackMinScoreMedian = 0.3,
          .fallbackMinDwellMs = 1000,
          .fallbackSuppressKnown = true};
}
}

TEST_CASE("the fallback gate drops a matched known person")
{
  CameraNotificationPolicy policy(fallbackConfig());
  const auto verdict = policy.fallbackDecision(
      fallbackEvent({.cameraId = 1,
                     .identityState = "known",
                     .scoreMedian = 0.9,
                     .scoreSamples = 4,
                     .dwellMs = 5000,
                     .withHistory = true}));
  CHECK(verdict == CameraNotificationPolicy::FallbackDecision::DropKnown);

  auto notifier = makeNotifier(fallbackConfig(), nullptr);
  notifier->handle(fallbackEvent({.cameraId = 1,
                                  .identityState = "known",
                                  .scoreMedian = 0.9,
                                  .scoreSamples = 4,
                                  .dwellMs = 5000,
                                  .withHistory = true}));
  const auto counts = notifier->policy().fallbackCounts();
  CHECK(counts.droppedKnown == 1);
  CHECK(counts.passed == 0);
}

TEST_CASE("the fallback gate reads the primary track guard would")
{
  CameraNotificationPolicy policy(fallbackConfig());

  const auto eventWithPrimary = [](int64_t primaryTrackId) {
    Json::Value event;
    event["cameraId"] = Json::Int64(9);
    event["cameraName"] = "Front door";
    event["rule"] = "person_in_alert_zone";
    event["severity"] = "critical";
    event["trackId"] = Json::Int64(primaryTrackId);

    Json::Value resident;
    resident["class"] = "person";
    resident["trackId"] = Json::Int64(21);
    resident["identityState"] = "known";
    resident["scoreMedian"] = 0.95;
    resident["scoreSamples"] = 4;
    resident["dwellMs"] = Json::Int64(9000);
    Json::Value bigBox;
    bigBox["w"] = 60.0;
    bigBox["h"] = 60.0;
    resident["bbox"] = bigBox;

    Json::Value intruder;
    intruder["class"] = "person";
    intruder["trackId"] = Json::Int64(22);
    intruder["identityState"] = "unrecognized";
    intruder["scoreMedian"] = 0.9;
    intruder["scoreSamples"] = 3;
    intruder["dwellMs"] = Json::Int64(4000);
    Json::Value smallBox;
    smallBox["w"] = 12.0;
    smallBox["h"] = 12.0;
    intruder["bbox"] = smallBox;

    Json::Value objects;
    objects.append(resident);
    objects.append(intruder);
    event["objects"] = objects;
    return event;
  };

  CHECK(policy.fallbackDecision(eventWithPrimary(22)) ==
        CameraNotificationPolicy::FallbackDecision::Notify);
  CHECK(policy.fallbackDecision(eventWithPrimary(21)) ==
        CameraNotificationPolicy::FallbackDecision::DropKnown);

  Json::Value weakIntruder = eventWithPrimary(22);
  weakIntruder["objects"][1]["scoreMedian"] = 0.2;
  CHECK(policy.fallbackDecision(weakIntruder) ==
        CameraNotificationPolicy::FallbackDecision::DropWeakScore);

  Json::Value briefIntruder = eventWithPrimary(22);
  briefIntruder["objects"][1]["dwellMs"] = Json::Int64(500);
  CHECK(policy.fallbackDecision(briefIntruder) ==
        CameraNotificationPolicy::FallbackDecision::DropShortDwell);

  auto notifier = makeNotifier(fallbackConfig(), nullptr);
  notifier->handle(eventWithPrimary(22));
  const auto counts = notifier->policy().fallbackCounts();
  CHECK(counts.droppedKnown == 0);
  CHECK(counts.passed == 1);
}

TEST_CASE("the fallback gate drops weak detector scores")
{
  CameraNotificationPolicy policy(fallbackConfig());
  const auto verdict = policy.fallbackDecision(
      fallbackEvent({.cameraId = 1,
                     .identityState = "unrecognized",
                     .scoreMedian = 0.2,
                     .scoreSamples = 3,
                     .dwellMs = 5000,
                     .withHistory = true}));
  CHECK(verdict == CameraNotificationPolicy::FallbackDecision::DropWeakScore);

  auto notifier = makeNotifier(fallbackConfig(), nullptr);
  notifier->handle(fallbackEvent({.cameraId = 1,
                                  .identityState = "unrecognized",
                                  .scoreMedian = 0.2,
                                  .scoreSamples = 3,
                                  .dwellMs = 5000,
                                  .withHistory = true}));
  CHECK(notifier->policy().fallbackCounts().droppedWeakScore == 1);
}

TEST_CASE("the fallback gate drops short dwells")
{
  CameraNotificationPolicy policy(fallbackConfig());
  const auto verdict = policy.fallbackDecision(
      fallbackEvent({.cameraId = 1,
                     .identityState = "unrecognized",
                     .scoreMedian = 0.9,
                     .scoreSamples = 4,
                     .dwellMs = 500,
                     .withHistory = true}));
  CHECK(verdict == CameraNotificationPolicy::FallbackDecision::DropShortDwell);

  auto notifier = makeNotifier(fallbackConfig(), nullptr);
  notifier->handle(fallbackEvent({.cameraId = 1,
                                  .identityState = "unrecognized",
                                  .scoreMedian = 0.9,
                                  .scoreSamples = 4,
                                  .dwellMs = 500,
                                  .withHistory = true}));
  CHECK(notifier->policy().fallbackCounts().droppedShortDwell == 1);
}

TEST_CASE("the fallback gate passes strong evidence and fails open")
{
  CameraNotificationPolicy policy(fallbackConfig());
  CHECK(policy.fallbackDecision(fallbackEvent({.cameraId = 1,
                                               .identityState = "unrecognized",
                                               .scoreMedian = 0.9,
                                               .scoreSamples = 4,
                                               .dwellMs = 5000,
                                               .withHistory = true})) ==
        CameraNotificationPolicy::FallbackDecision::Notify);
  CHECK(policy.fallbackDecision(fallbackEvent({.cameraId = 1,
                                               .identityState = {},
                                               .scoreMedian = 0.0,
                                               .scoreSamples = 0,
                                               .dwellMs = 0,
                                               .withHistory = false})) ==
        CameraNotificationPolicy::FallbackDecision::Notify);

  CameraNotificationPolicy::Config strict = fallbackConfig();
  strict.fallbackMinScoreMedian = 0.95;
  CameraNotificationPolicy strictPolicy(strict);
  CHECK(strictPolicy.fallbackDecision(
            fallbackEvent({.cameraId = 1,
                           .identityState = "unrecognized",
                           .scoreMedian = 0.9,
                           .scoreSamples = 4,
                           .dwellMs = 5000,
                           .withHistory = true})) ==
        CameraNotificationPolicy::FallbackDecision::DropWeakScore);
}

TEST_CASE("every fallback drop lands a durable row in the notification store")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  auto notifier = makeNotifier(fallbackConfig(), nullptr);

  notifier->handle(fallbackEvent({.cameraId = 11,
                                  .identityState = "known",
                                  .scoreMedian = 0.9,
                                  .scoreSamples = 4,
                                  .dwellMs = 5000,
                                  .withHistory = true}));
  notifier->handle(fallbackEvent({.cameraId = 12,
                                  .identityState = "unrecognized",
                                  .scoreMedian = 0.2,
                                  .scoreSamples = 3,
                                  .dwellMs = 5000,
                                  .withHistory = true}));
  notifier->handle(fallbackEvent({.cameraId = 13,
                                  .identityState = "unrecognized",
                                  .scoreMedian = 0.9,
                                  .scoreSamples = 4,
                                  .dwellMs = 500,
                                  .withHistory = true}));
  Json::Value soft = fallbackEvent({.cameraId = 14,
                                    .identityState = "unrecognized",
                                    .scoreMedian = 0.9,
                                    .scoreSamples = 4,
                                    .dwellMs = 5000,
                                    .withHistory = true});
  soft["rule"] = "person_day";
  soft["severity"] = "info";
  notifier->handle(soft);

  const std::string scoped =
      "SELECT COUNT(*) AS total FROM camera_fallback_event WHERE camera_id IN "
      "(11, 12, 13, 14)";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline &&
         countRows(scoped) < 4)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(countRows(scoped) == 4);
  CHECK(fallbackReason(11, "person_in_alert_zone") == "drop_known");
  CHECK(fallbackReason(12, "person_in_alert_zone") == "drop_weak_score");
  CHECK(fallbackReason(13, "person_in_alert_zone") == "drop_short_dwell");
  CHECK(fallbackReason(14, "person_day") == "non_hard_signal");
}

TEST_CASE("fallback logging degrades when its table is unavailable")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  DbService::client()->execSqlSync(
      "ALTER TABLE camera_fallback_event RENAME TO camera_fallback_event_hidden");
  const CameraFallbackLogRepository repository;
  CHECK_FALSE(drogon::sync_wait(repository.log(
      {.cameraId = 1,
       .rule = "person_in_alert_zone",
       .severity = "critical",
       .reason = FallbackDropReason::DropKnown,
       .createdAt = 1})));
  CHECK(drogon::sync_wait(repository.purgeOlderThan(2)) == 0);
  DbService::client()->execSqlSync(
      "ALTER TABLE camera_fallback_event_hidden RENAME TO camera_fallback_event");
}

TEST_CASE("fallback retention purges only old rows")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  CHECK(camera_notifier::resolveConfig().fallbackRetentionDays == 90);
  const CameraFallbackLogRepository repository;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  REQUIRE(drogon::sync_wait(repository.log(
      {.cameraId = 21,
       .rule = "person_in_alert_zone",
       .severity = "critical",
       .reason = FallbackDropReason::NonHardSignal,
       .createdAt = now - 100 * 86400})));
  REQUIRE(drogon::sync_wait(repository.log(
      {.cameraId = 22,
       .rule = "person_in_alert_zone",
       .severity = "critical",
       .reason = FallbackDropReason::NonHardSignal,
       .createdAt = now})));
  CHECK(drogon::sync_wait(repository.purgeOlderThan(now - 90 * 86400)) == 1);
  CHECK(countRows("SELECT COUNT(*) AS total FROM camera_fallback_event WHERE "
                  "camera_id = 21") == 0);
  CHECK(countRows("SELECT COUNT(*) AS total FROM camera_fallback_event WHERE "
                  "camera_id = 22") == 1);
}
