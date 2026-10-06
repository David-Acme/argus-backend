#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/module-request/services/notification-module-request.hxx>
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
  TempDb db_{"module-request-test-notification"};
  std::optional<AppRunner> runner_;
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

struct Member
{
  int64_t id;
  std::string name;
  std::string role;
  std::string lang;
  bool active{true};
};

argus::identity::v1::UserIdentity user(const Member& member)
{
  argus::identity::v1::UserIdentity identity;
  identity.set_user_id(member.id);
  identity.set_name(member.name);
  identity.set_lang(member.lang);
  identity.set_role(member.role);
  identity.set_is_active(member.active);
  return identity;
}

class FixedHousehold final : public IdentityClient
{
public:
  explicit FixedHousehold(std::optional<std::vector<argus::identity::v1::UserIdentity>> users)
      : IdentityClient("127.0.0.1:1", "notification-identity"), users_(std::move(users))
  {
  }

  [[nodiscard]] std::optional<std::vector<argus::identity::v1::UserIdentity>> listUsers() const override
  {
    return users_;
  }

private:
  std::optional<std::vector<argus::identity::v1::UserIdentity>> users_;
};

int64_t rows(const std::string& where)
{
  return DbService::client()->execSqlSync("SELECT COUNT(*) AS total FROM notification WHERE " + where).front()["total"].as<int64_t>();
}

ModuleRequestInput asked(const std::string& day, int64_t userId = 5)
{
  return {.moduleId = "surveillance",
          .moduleName = {.es = "Vigilancia", .en = "Surveillance"},
          .userId = userId,
          .day = day};
}
}

TEST_CASE("a module request notifies every active Owner in their language, once per person, module and day")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  const auto household = std::make_shared<FixedHousehold>(std::vector<argus::identity::v1::UserIdentity>{
      user({.id = 1, .name = "Olga", .role = "owner", .lang = "es"}),
      user({.id = 2, .name = "Omar", .role = "owner", .lang = "en"}),
      user({.id = 3, .name = "Otto", .role = "owner", .lang = "es", .active = false}),
      user({.id = 5, .name = "Rita", .role = "resident", .lang = "es"}),
      user({.id = 6, .name = "Gus", .role = "guard", .lang = "es"})});
  NotificationModuleRequest host({.identity = household, .delivery = {}});

  const auto first = host.request(asked("2026-10-06"));
  CHECK(first.notified == 2);
  CHECK_FALSE(first.duplicate);
  CHECK(rows("type = 'module_request'") == 2);
  CHECK(rows("user_id = 3") == 0);
  CHECK(rows("user_id = 5") == 0);

  const auto spanish = DbService::client()->execSqlSync("SELECT title, body, data FROM notification WHERE user_id = 1");
  REQUIRE(spanish.size() == 1);
  CHECK(spanish.front()["title"].as<std::string>() == "Piden un módulo");
  CHECK(spanish.front()["body"].as<std::string>() == "Rita quiere usar el módulo Vigilancia. ¿Lo activas?");
  const auto data = json_util::fromString(spanish.front()["data"].as<std::string>());
  CHECK(data["kind"].asString() == "module_request");
  CHECK(data["moduleId"].asString() == "surveillance");
  CHECK(data["requestedBy"].asInt64() == 5);
  CHECK(data["requestedByName"].asString() == "Rita");
  CHECK(data["action"].asString() == "enable_module");
  CHECK(data["threadKey"].asString() == "module_request:surveillance:5:2026-10-06");
  CHECK(data["urgency"].asString() == "active");

  const auto english = DbService::client()->execSqlSync("SELECT title, body FROM notification WHERE user_id = 2");
  REQUIRE(english.size() == 1);
  CHECK(english.front()["title"].as<std::string>() == "Module request");
  CHECK(english.front()["body"].as<std::string>() == "Rita would like to use the Surveillance module. Turn it on?");

  const auto again = host.request(asked("2026-10-06"));
  CHECK(again.notified == 0);
  CHECK(again.duplicate);
  CHECK(rows("type = 'module_request'") == 2);

  const auto tomorrow = host.request(asked("2026-10-07"));
  CHECK(tomorrow.notified == 2);
  CHECK_FALSE(tomorrow.duplicate);
  const auto other = host.request(asked("2026-10-06", 6));
  CHECK(other.notified == 2);
  CHECK(rows("type = 'module_request'") == 6);
}

TEST_CASE("a module request without an identity roster fails instead of telling nobody")
{
  SharedBoot& boot = sharedBoot();
  REQUIRE(boot.applySchema());
  NotificationModuleRequest withoutIdentity({.identity = nullptr, .delivery = {}});
  CHECK_THROWS(static_cast<void>(withoutIdentity.request(asked("2026-10-08"))));
  NotificationModuleRequest unreadable({.identity = std::make_shared<FixedHousehold>(std::nullopt), .delivery = {}});
  CHECK_THROWS(static_cast<void>(unreadable.request(asked("2026-10-08"))));
  NotificationModuleRequest alone(
      {.identity = std::make_shared<FixedHousehold>(std::vector<argus::identity::v1::UserIdentity>{
           user({.id = 5, .name = "Rita", .role = "resident", .lang = "es"})}),
       .delivery = {}});
  const auto nobody = alone.request(asked("2026-10-08"));
  CHECK(nobody.notified == 0);
  CHECK_FALSE(nobody.duplicate);
}
