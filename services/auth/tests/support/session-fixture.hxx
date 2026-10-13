#pragma once

#include <argus/auth/v1/auth.grpc.pb.h>
#include <atomic>
#include <auth/auth-client.hxx>
#include <auth/jwt-service.hxx>
#include <chrono>
#include <config/config-service.hxx>
#include <cstdint>
#include <cstdio>
#include <drogon/drogon.h>
#include <identity/identity-client.hxx>
#include <memory>
#include <optional>
#include <sqlite/db-service.hxx>
#include <string>
#include <text/sha256.hxx>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_AUTH_SCHEMA_PATH
#error "ARGUS_AUTH_SCHEMA_PATH must point at the auth schema.sql"
#endif

inline constexpr const char* kJwtSecret =
    "argus-auth-session-verdict-secret-0123456789";
inline constexpr const char* kRefreshSecret =
    "argus-auth-session-verdict-refresh-secret-0123456789";
inline constexpr const char* kFleetSecret =
    "argus-auth-fleet-secret-0123456789abcdef";
inline constexpr const char* kDeviceSecret = "00112233445566778899aabbccddeeff";
inline constexpr const char* kDeviceHash = "device-hash-primary";
inline constexpr const char* kOtherDeviceHash = "device-hash-other";
inline constexpr int64_t kUserId = 42;
inline constexpr int64_t kGatedUserId = 43;
inline constexpr int64_t kHourSeconds = 3600;

[[nodiscard]] inline int tempCounter()
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

inline void setConfig()
{
  ConfigService::setRuntimeString("jwt.secret", kJwtSecret);
  ConfigService::setRuntimeString("jwt.refresh_secret", kRefreshSecret);
  ConfigService::setRuntimeString("jwt.access_ttl_minutes", "60");
  ConfigService::setRuntimeString("jwt.refresh_ttl_days", "7");
}

class ScriptedIdentityClient : public IdentityClient
{
public:
  ScriptedIdentityClient() : IdentityClient("127.0.0.1:1") {}

  [[nodiscard]] std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const override
  {
    calls.fetch_add(1);
    if (!reachable)
      return std::nullopt;

    argus::identity::v1::GetUserResponse response;
    auto* user = response.mutable_user();
    user->set_user_id(userId);
    user->set_name(name);
    user->set_last_name(lastName);
    user->set_lang(lang);
    user->set_role(role);
    user->set_is_active(isActive);
    return response;
  }

  mutable std::atomic<int> calls{0};
  bool reachable{true};
  bool isActive{true};
  std::string name{"Ada"};
  std::string lastName{"Rico"};
  std::string lang{"es"};
  std::string role{"owner"};
};

class Fixture
{
public:
  Fixture() = default;

  ~Fixture()
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

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  [[nodiscard]] bool start()
  {
    if (started_)
      return ready_;
    started_ = true;
    setConfig();
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                         .filename = db_.path(),
                                                         .name = "default",
                                                         .timeout = -1});
    runner_ = std::thread([] { drogon::app().run(); });
    if (!waitForBoot())
      return false;
    ready_ = DbService::runScriptFile(ARGUS_AUTH_SCHEMA_PATH);
    return ready_;
  }

  [[nodiscard]] ScriptedIdentityClient& identity() { return identity_; }

private:
  [[nodiscard]] static bool waitForBoot()
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      if (drogon::app().isRunning())
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return drogon::app().isRunning();
  }

  TempDb db_{"auth-session-verdict-test"};
  ScriptedIdentityClient identity_;
  std::thread runner_;
  bool started_{false};
  bool ready_{false};
};

[[nodiscard]] inline std::unique_ptr<Fixture>& fixtureStorage()
{
  static std::unique_ptr<Fixture> booted;
  return booted;
}

[[nodiscard]] inline Fixture& fixture()
{
  auto& booted = fixtureStorage();
  if (!booted)
    booted = std::make_unique<Fixture>();
  return *booted;
}

inline void stopFixture()
{
  fixtureStorage().reset();
}

struct SessionSeed
{
  int64_t userId{kUserId};
  std::string deviceHash{kDeviceHash};
  int64_t expiresAt{0};
  bool hashed{true};
};

[[nodiscard]] inline std::string issueToken(int64_t userId)
{
  static std::atomic<int> issued{0};
  JwtService jwt{JwtRole::Issuer};
  return jwt.generateAccess({{"sub", std::to_string(userId)},
                             {"iss", "argus"},
                             {"jti", std::to_string(issued.fetch_add(1))}});
}

[[nodiscard]] inline std::string seedSession(const SessionSeed& seed)
{
  const std::string token = issueToken(seed.userId);
  const std::string refresh = "refresh-" + token;
  std::string storedAccess =
      seed.hashed ? argus::hash::sha256Hex(token) : token;
  std::string storedRefresh =
      seed.hashed ? argus::hash::sha256Hex(refresh) : refresh;
  DbService::client()->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, is_valid, is_used, expires_at) "
      "VALUES (?, ?, ?, ?, '', 1, 0, ?)",
      seed.userId, storedAccess, storedRefresh, seed.deviceHash,
      seed.expiresAt);
  return token;
}

struct Verdict
{
  bool valid{false};
  std::string reason;
  std::string name;
  std::string role;
  std::string lang;
  int64_t expiresAt{0};
  bool answered{false};
};

[[nodiscard]] inline Verdict ask(const AuthClient& client,
                                 const std::string& accessToken)
{
  const auto answer = client.validateToken({.accessToken = accessToken,
                                            .deviceHash = kDeviceHash,
                                            .hasDeviceContext = true,
                                            .origin = {}});
  if (!answer.has_value())
    return Verdict{};

  Verdict verdict;
  verdict.answered = true;
  verdict.valid = answer->valid();
  verdict.reason = answer->reason();
  verdict.expiresAt = answer->expires_at();
  if (answer->has_user()) {
    verdict.name = answer->user().name();
    verdict.role = answer->user().role();
    verdict.lang = answer->user().lang();
  }
  return verdict;
}
