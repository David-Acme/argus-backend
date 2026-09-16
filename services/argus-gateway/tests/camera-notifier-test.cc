#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <notification/notification-client.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <sync/camera-notifier.hxx>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kIdentityDb = "camera-notifier-test-identity.db";
constexpr const char* kGatewayDb = "camera-notifier-test-gateway.db";

bool waitForFallbackRows(int64_t expected, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto rows = DbService::gatewayClient()->execSqlSync(
        "SELECT COUNT(*) AS total FROM gateway_fallback_event");
    if (!rows.empty() && rows.front()["total"].as<int64_t>() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string fallbackReason(int64_t cameraId, const std::string& rule)
{
  const auto rows = DbService::gatewayClient()->execSqlSync(
      "SELECT reason FROM gateway_fallback_event WHERE camera_id = ? AND "
      "rule = ? ORDER BY id DESC LIMIT 1",
      cameraId, rule);
  if (rows.empty())
    return {};
  return rows.front()["reason"].as<std::string>();
}

// A timestamp whose LOCAL hour is the requested one (hourOfDay reads the clock).
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
  Json::Value objects;
  Json::Value object;
  object["class"] = "person";
  object["confidence"] = 0.9;
  objects.append(object);
  event["objects"] = objects;
  return event;
}

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

void removeDbFiles(const char* base)
{
  std::remove(base);
  std::remove((std::string(base) + "-wal").c_str());
  std::remove((std::string(base) + "-shm").c_str());
}

struct SharedBoot
{
  std::thread runner;

  SharedBoot()
  {
    removeDbFiles(kIdentityDb);
    removeDbFiles(kGatewayDb);
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, kIdentityDb, "default", -1});
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, kGatewayDb, "gateway", -1});
    runner = std::thread([] { drogon::app().run(); });
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    DbService::setGatewayClient(drogon::app().getDbClient("gateway"));
    if (!DbService::runScriptFile(ARGUS_GATEWAY_SCHEMA_PATH,
                                  DbService::gatewayClient()))
      throw std::runtime_error("gateway schema apply failed");
  }

  ~SharedBoot()
  {
    drogon::app().quit();
    if (runner.joinable())
      runner.join();
    removeDbFiles(kIdentityDb);
    removeDbFiles(kGatewayDb);
  }
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

// Records the create requests the notifier hands to the notification SDK.
class RecordingNotificationClient final : public NotificationClient
{
public:
  RecordingNotificationClient()
      : NotificationClient(
            {.target = "127.0.0.1:1", .credential = "gateway-notif"})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    std::lock_guard lock(mutex_);
    requests.push_back(request);
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  int totalUsers() const
  {
    std::lock_guard lock(mutex_);
    int total = 0;
    for (const auto& request : requests)
      total += request.user_ids_size();
    return total;
  }

  bool waitForUsers(int expected, std::chrono::milliseconds timeout) const
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (totalUsers() >= expected)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return totalUsers() >= expected;
  }

  mutable std::mutex mutex_;
  mutable std::vector<argus::notification::v1::CreateNotificationsRequest>
      requests;
};
} // namespace

TEST_CASE("the notification budget allows budget_per_hour then suppresses")
{
  CameraNotificationPolicy policy({2, -1, -1, 30000});
  const int64_t start = atLocalHour({.hour = 12, .minute = 0, .day = 15});

  CHECK(policy.shouldNotify(1, start));
  CHECK(policy.shouldNotify(1, start + 1000));
  CHECK_FALSE(policy.shouldNotify(1, start + 2000));

  // A new rolling hour resets the budget; the other camera is independent.
  CHECK(policy.shouldNotify(1, start + 3600000));
  CHECK(policy.shouldNotify(2, start + 1000));
}

TEST_CASE("silent hours suppress and support wrapping")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 23, .minute = 0, .day = 15})));
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 2, .minute = 0, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 12, .minute = 0, .day = 15})));
  // 21:59 is still before the silent window opens.
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 21, .minute = 59, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 6, .minute = 0, .day = 15})));

  CameraNotificationPolicy disabled({6, -1, -1, 30000});
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

  // Still inside the hour: nothing is flushed yet.
  CHECK(policy.takeDigest(1, start + 1000).empty());

  const std::string digest = policy.takeDigest(1, start + 3600000);
  CHECK(digest.find("3 events suppressed") != std::string::npos);
  CHECK(digest.find("2 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);

  // The counters reset after a digest is taken.
  CHECK(policy.takeDigest(1, start + 7200000).empty());
}

TEST_CASE("a digest flushes when the silent window ends")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  const int64_t night = atLocalHour({.hour = 23, .minute = 0, .day = 15});

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  // Morning after the silent window [22, 6) closed: the digest goes out.
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

  // An event arriving right after the roll must not erase the digest.
  CHECK(policy.shouldNotify(1, start + 3600000));
  const std::string digest = policy.takeDigest(1, start + 3600001);
  CHECK(digest.find("2 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);

  // The digest is taken once; a second read finds nothing.
  CHECK(policy.takeDigest(1, start + 3600002).empty());
}

TEST_CASE("counts suppressed inside silent hours carry until the window ends")
{
  CameraNotificationPolicy policy({6, 22, 6, 30000});
  const int64_t night = atLocalHour({.hour = 23, .minute = 0, .day = 15});

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  // The budget hour rolls inside the silent window; the counts carry.
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 0, .minute = 5, .day = 16})));
  policy.countSuppressed(1, "car");
  CHECK(policy.takeDigest(1, atLocalHour({.hour = 1, .minute = 0, .day = 16})).empty());

  // Morning after the silent window [22, 6) closed: everything flushes.
  const std::string digest = policy.takeDigest(1, atLocalHour({.hour = 6, .minute = 0, .day = 16}));
  CHECK(digest.find("2 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
  CHECK(digest.find("1 car") != std::string::npos);
}

TEST_CASE("the guard heartbeat gates the raw fallback window")
{
  CameraNotificationPolicy policy({6, -1, -1, 30000});
  CHECK_FALSE(policy.guardReady(1000));
  policy.markGuardHeartbeat(1000);
  CHECK(policy.guardReady(1000));
  CHECK(policy.guardReady(31000));
  CHECK_FALSE(policy.guardReady(31001));
}

TEST_CASE("the consumer applies the budget and creates camera notifications")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  auto client = DbService::client();
  // Identity user table only: the notification write leaves through the SDK.
  client->execSqlSync("DROP TABLE IF EXISTS user");
  client->execSqlSync(
      "CREATE TABLE user ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL, last_name TEXT NOT NULL, "
      "role TEXT NOT NULL CHECK (role IN ('owner', 'resident', 'guard', 'guest')), "
      "lang TEXT NOT NULL DEFAULT 'es', "
      "is_active INTEGER NOT NULL DEFAULT 1, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  // One owner and one guard (notified), one resident and one deactivated (skipped).
  client->execSqlSync("INSERT INTO user (name, last_name, role) VALUES "
                      "('Ana', 'Owner', 'owner')");
  client->execSqlSync("INSERT INTO user (name, last_name, role) VALUES "
                      "('Gus', 'Guard', 'guard')");
  client->execSqlSync("INSERT INTO user (name, last_name, role) VALUES "
                      "('Resi', 'Dent', 'resident')");
  client->execSqlSync("INSERT INTO user (name, last_name, role, is_active) "
                      "VALUES ('Old', 'Owner', 'owner', 0)");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  auto notificationClient = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier({6, -1, -1, 30000}, notificationClient);

  notifier.handle(eventJson({.cameraId = 1,
                             .rule = "person_in_alert_zone",
                             .severity = "critical"}));
  REQUIRE(notificationClient->waitForUsers(2, std::chrono::seconds(10)));
  {
    std::lock_guard lock(notificationClient->mutex_);
    REQUIRE(notificationClient->requests.size() == 1);
    const auto& request = notificationClient->requests.front();
    REQUIRE(request.user_ids_size() == 2);
    CHECK(request.user_ids(0) == 1);
    CHECK(request.user_ids(1) == 2);
    CHECK(request.type() == "camera");
    CHECK(request.title() == "Front door: person_in_alert_zone");
    CHECK(request.body() == "Severity critical; detected person");
    CHECK(json_util::fromString(request.data())["cameraId"].asInt64() == 1);
  }

  // Budget 6 per hour: the next five pass, the seventh is suppressed.
  for (int i = 0; i < 5; ++i)
    notifier.handle(eventJson({.cameraId = 1,
                               .rule = "person_in_alert_zone",
                               .severity = "critical"}));
  REQUIRE(notificationClient->waitForUsers(12, std::chrono::seconds(10)));
  notifier.handle(eventJson({.cameraId = 1,
                             .rule = "person_in_alert_zone",
                             .severity = "critical"}));

  // Wait past any in-flight delivery, then confirm the count stopped at 12.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationClient->totalUsers() == 12);

  // Without a guard heartbeat only hard signals reach the raw fallback.
  notifier.handle(
      eventJson({.cameraId = 1, .rule = "person_day", .severity = "info"}));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationClient->totalUsers() == 12);

  // A malformed payload is dropped without touching the SDK.
  notifier.handle(json_util::fromString("[1, 2, 3]"));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationClient->totalUsers() == 12);
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
} // namespace

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

  auto client = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier(fallbackConfig(), client);
  notifier.handle(fallbackEvent({.cameraId = 1,
                                 .identityState = "known",
                                 .scoreMedian = 0.9,
                                 .scoreSamples = 4,
                                 .dwellMs = 5000,
                                 .withHistory = true}));
  CHECK(client->totalUsers() == 0);
  const auto counts = notifier.policy().fallbackCounts();
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

  CameraObjectNotifier notifier(fallbackConfig(), nullptr);
  notifier.handle(eventWithPrimary(22));
  const auto counts = notifier.policy().fallbackCounts();
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

  auto client = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier(fallbackConfig(), client);
  notifier.handle(fallbackEvent({.cameraId = 1,
                                 .identityState = "unrecognized",
                                 .scoreMedian = 0.2,
                                 .scoreSamples = 3,
                                 .dwellMs = 5000,
                                 .withHistory = true}));
  CHECK(client->totalUsers() == 0);
  CHECK(notifier.policy().fallbackCounts().droppedWeakScore == 1);
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

  auto client = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier(fallbackConfig(), client);
  notifier.handle(fallbackEvent({.cameraId = 1,
                                 .identityState = "unrecognized",
                                 .scoreMedian = 0.9,
                                 .scoreSamples = 4,
                                 .dwellMs = 500,
                                 .withHistory = true}));
  CHECK(client->totalUsers() == 0);
  CHECK(notifier.policy().fallbackCounts().droppedShortDwell == 1);
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

TEST_CASE("every fallback drop lands a durable row in the gateway store")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  auto client = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier(fallbackConfig(), client);

  notifier.handle(fallbackEvent({.cameraId = 11,
                                 .identityState = "known",
                                 .scoreMedian = 0.9,
                                 .scoreSamples = 4,
                                 .dwellMs = 5000,
                                 .withHistory = true}));
  notifier.handle(fallbackEvent({.cameraId = 12,
                                 .identityState = "unrecognized",
                                 .scoreMedian = 0.2,
                                 .scoreSamples = 3,
                                 .dwellMs = 5000,
                                 .withHistory = true}));
  notifier.handle(fallbackEvent({.cameraId = 13,
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
  notifier.handle(soft);

  REQUIRE(waitForFallbackRows(4, std::chrono::seconds(10)));
  CHECK(fallbackReason(11, "person_in_alert_zone") == "drop_known");
  CHECK(fallbackReason(12, "person_in_alert_zone") == "drop_weak_score");
  CHECK(fallbackReason(13, "person_in_alert_zone") == "drop_short_dwell");
  CHECK(fallbackReason(14, "person_day") == "non_hard_signal");
  CHECK(client->totalUsers() == 0);
}

TEST_CASE("fallback logging degrades when the store is unavailable")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  const std::string bare = "camera-notifier-test-bare.db";
  std::remove(bare.c_str());
  auto saved = DbService::gatewayClient();
  DbService::setGatewayClient(
      drogon::orm::DbClient::newSqlite3Client("filename=" + bare, 1));
  FallbackLogRepository repository;
  CHECK_FALSE(drogon::sync_wait(repository.log(
      {.cameraId = 1,
       .rule = "person_in_alert_zone",
       .severity = "critical",
       .reason = FallbackDropReason::DropKnown,
       .createdAt = 1})));
  CHECK(drogon::sync_wait(repository.purgeOlderThan(2)) == 0);
  DbService::setGatewayClient(saved);
  std::remove(bare.c_str());
  std::remove((bare + "-wal").c_str());
  std::remove((bare + "-shm").c_str());
}

TEST_CASE("fallback retention purges only old rows")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  CHECK(camera_notifier::resolveConfig().fallbackRetentionDays == 90);
  FallbackLogRepository repository;
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
  const auto oldRows = DbService::gatewayClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM gateway_fallback_event WHERE camera_id = "
      "21");
  CHECK(oldRows.front()["total"].as<int64_t>() == 0);
  const auto freshRows = DbService::gatewayClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM gateway_fallback_event WHERE camera_id = "
      "22");
  CHECK(freshRows.front()["total"].as<int64_t>() == 1);
}
