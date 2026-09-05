#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <sync/camera-notifier.hxx>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

namespace
{
constexpr const char* kIdentityDb = "camera-notifier-test-identity.db";

// A timestamp whose LOCAL hour is the requested one (hourOfDay reads the
// local clock, so the tests build their timestamps with mktime).
int64_t atLocalHour(int hour, int minute = 0, int day = 15)
{
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

Json::Value eventJson(int64_t cameraId, const char* rule, const char* severity)
{
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

int notificationCount()
{
  const auto rows = drogon::app().getDbClient()->execSqlSync(
      "SELECT COUNT(*) AS n FROM notification WHERE type = 'camera'");
  return rows.front()["n"].as<int>();
}

bool waitForNotifications(int expected, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (notificationCount() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return notificationCount() >= expected;
}
} // namespace

TEST_CASE("the notification budget allows budget_per_hour then suppresses")
{
  CameraNotificationPolicy policy({2, -1, -1});
  const int64_t start = atLocalHour(12);

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
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour(23)));
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour(2)));
  CHECK(policy.shouldNotify(1, atLocalHour(12)));
  // 21:59 is still before the silent window opens.
  CHECK(policy.shouldNotify(1, atLocalHour(21, 59)));
  CHECK(policy.shouldNotify(1, atLocalHour(6, 0)));

  CameraNotificationPolicy disabled({6, -1, -1});
  CHECK(disabled.shouldNotify(1, atLocalHour(23)));
}

TEST_CASE("the digest summarizes suppressed events after the window closes")
{
  CameraNotificationPolicy policy({1, -1, -1});
  const int64_t start = atLocalHour(12);

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
  const int64_t night = atLocalHour(23);

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  // Morning after the silent window [22, 6) closed: the digest goes out.
  const std::string digest = policy.takeDigest(1, atLocalHour(6, 0, 16));
  CHECK(digest.find("1 events suppressed") != std::string::npos);
  CHECK(digest.find("1 person") != std::string::npos);
}

TEST_CASE("a pending digest survives the hour-roll race")
{
  CameraNotificationPolicy policy({6, -1, -1});
  const int64_t start = atLocalHour(12);

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
  const int64_t night = atLocalHour(23);

  CHECK_FALSE(policy.shouldNotify(1, night));
  policy.countSuppressed(1, "person");

  // The budget hour rolls inside the silent window; the digest must not
  // go out at night and the counts carry.
  CHECK_FALSE(policy.shouldNotify(1, atLocalHour(0, 5, 16)));
  policy.countSuppressed(1, "car");
  CHECK(policy.takeDigest(1, atLocalHour(1, 0, 16)).empty());

  // Morning after the silent window [22, 6) closed: everything flushes.
  const std::string digest = policy.takeDigest(1, atLocalHour(6, 0, 16));
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
  // Identity user table (identity-schema.sql shape) plus the notification
  // table the service writes (argus.db shape).
  client->execSqlSync(
      "CREATE TABLE user ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL, last_name TEXT NOT NULL, "
      "role TEXT NOT NULL CHECK (role IN ('owner', 'resident', 'guard', 'guest')), "
      "lang TEXT NOT NULL DEFAULT 'es', "
      "is_active INTEGER NOT NULL DEFAULT 1, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
      "CREATE TABLE notification ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
      "type TEXT NOT NULL DEFAULT 'system', "
      "title TEXT NOT NULL DEFAULT '', body TEXT NOT NULL DEFAULT '', "
      "data TEXT NOT NULL DEFAULT '{}', "
      "is_read INTEGER NOT NULL DEFAULT 0 CHECK (is_read IN (0, 1)), "
      "read_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  // One owner (notified), one guard (notified), one resident (skipped) and
  // one deactivated owner (skipped).
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

  CameraObjectNotifier notifier({6, -1, -1});

  notifier.handle(eventJson(1, "person_in_alert_zone", "critical"));
  REQUIRE(waitForNotifications(2, std::chrono::seconds(10)));
  const auto rows = drogon::app().getDbClient()->execSqlSync(
      "SELECT user_id, title, body FROM notification WHERE type = 'camera' "
      "ORDER BY user_id");
  REQUIRE(rows.size() == 2);
  CHECK(rows[0]["user_id"].as<int64_t>() == 1);
  CHECK(rows[1]["user_id"].as<int64_t>() == 2);
  CHECK(rows[0]["title"].as<std::string>() ==
        "Front door: person_in_alert_zone");
  CHECK(rows[0]["body"].as<std::string>() ==
        "Severity critical; detected person");

  // Budget 6 per hour: the next five pass, the seventh is suppressed.
  for (int i = 0; i < 5; ++i)
    notifier.handle(eventJson(1, "person_day", "info"));
  REQUIRE(waitForNotifications(12, std::chrono::seconds(10)));
  notifier.handle(eventJson(1, "person_day", "info"));

  // Wait past any in-flight write, then confirm the count stopped at 12.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationCount() == 12);

  // A malformed payload is dropped without touching the database.
  notifier.handle(json_util::fromString("[1, 2, 3]"));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(notificationCount() == 12);

  drogon::app().quit();
  runner.join();
  std::remove(kIdentityDb);
}
