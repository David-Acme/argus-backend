#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <notification/notification-client.hxx>
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

// Records the create requests the notifier hands to the notification SDK.
class RecordingNotificationClient final : public NotificationClient
{
public:
  RecordingNotificationClient() : NotificationClient("127.0.0.1:1") {}

  std::optional<argus::notification::v1::CreateNotificationsResponse>
  createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    std::lock_guard lock(mutex_);
    requests.push_back(request);
    argus::notification::v1::CreateNotificationsResponse response;
    response.set_created(request.user_ids_size());
    return response;
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
  CameraNotificationPolicy policy({2, -1, -1});
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
  CameraNotificationPolicy policy({6, 22, 6});
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 23, .minute = 0, .day = 15})));
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour({.hour = 2, .minute = 0, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 12, .minute = 0, .day = 15})));
  // 21:59 is still before the silent window opens.
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 21, .minute = 59, .day = 15})));
  CHECK(policy.shouldNotify(1, atLocalHour({.hour = 6, .minute = 0, .day = 15})));

  CameraNotificationPolicy disabled({6, -1, -1});
  CHECK(disabled.shouldNotify(1, atLocalHour({.hour = 23, .minute = 0, .day = 15})));
}

TEST_CASE("the digest summarizes suppressed events after the window closes")
{
  CameraNotificationPolicy policy({1, -1, -1});
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
  CameraNotificationPolicy policy({6, 22, 6});
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
  CameraNotificationPolicy policy({6, 22, 6});
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

TEST_CASE("the consumer applies the budget and creates camera notifications")
{
  std::remove(kIdentityDb);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") +
                                                  kIdentityDb,
                                              1);
  // Identity user table only: the notification write leaves through the SDK.
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
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kIdentityDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  auto notificationClient = std::make_shared<RecordingNotificationClient>();
  CameraObjectNotifier notifier({6, -1, -1}, notificationClient);

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
    notifier.handle(eventJson({.cameraId = 1, .rule = "person_day", .severity = "info"}));
  REQUIRE(notificationClient->waitForUsers(12, std::chrono::seconds(10)));
  notifier.handle(eventJson({.cameraId = 1, .rule = "person_day", .severity = "info"}));

  // Wait past any in-flight delivery, then confirm the count stopped at 12.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationClient->totalUsers() == 12);

  // A malformed payload is dropped without touching the SDK.
  notifier.handle(json_util::fromString("[1, 2, 3]"));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationClient->totalUsers() == 12);

  drogon::app().quit();
  runner.join();
  std::remove(kIdentityDb);
}
