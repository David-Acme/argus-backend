#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/api/auth/services/auth-service.hxx>
#include <feature/rpc/identity-rpc.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <config/config-service.hxx>
#include <auth/jwt-service.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <errors/response-exception.hxx>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <auth/request-context.hxx>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
// A filter's coroutine driven straight by the test skips the filter advice, so a
// refusal reaches the caller as the thrown refusal itself: what the advice would
// have formatted into a status and a body is read off the exception here.
struct Refusal
{
  int status;
  std::string code;
  std::string message;
};

std::optional<Refusal> refusalOf(drogon::Task<drogon::HttpResponsePtr> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{error.statusCode(), error.errorCode(),
                   std::string(error.what())};
  }
  return std::nullopt;
}

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

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

constexpr const char* kJwtSecret =
    "f5-2-device-credential-test-jwt-secret-0123456789";
constexpr const char* kFingerprintSecret = "f5-2-test-secret";
constexpr const char* kUa = "argus-ua/1.0";
constexpr const char* kDesktopUa = "argus-desktop/1.0";
constexpr const char* kSecret = "00112233445566778899aabbccddeeff"
                               "00112233445566778899aabbccddeeff";
constexpr const char* kFleetSecret = "f7-r-fleet-secret-0123456789abcdef";

void setConfig()
{
  ConfigService::setRuntimeString("jwt.secret", kJwtSecret);
  ConfigService::setRuntimeString("jwt.refresh_secret", kJwtSecret);
  ConfigService::setRuntimeString("jwt.access_ttl_minutes", "60");
  ConfigService::setRuntimeString("jwt.refresh_ttl_days", "7");
  ConfigService::setRuntimeString("device.fingerprint_secret",
                                  kFingerprintSecret);
  ConfigService::setRuntimeString("device.trust_forwarded_for", "true");
  ConfigService::setRuntimeString(
      "device.trusted_proxy_ips",
      drogon::HttpRequest::newHttpRequest()->getPeerAddr().toIp());
}

// Varies the source IP the filter resolves through the trusted proxy path.
void setSourceIp(const drogon::HttpRequestPtr& req, const std::string& ip)
{
  req->addHeader("X-Forwarded-For", ip);
}

const DeviceContext& deviceCtx(const drogon::HttpRequestPtr& req)
{
  return req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
}

// Seed through the sqlite3 C API. A throwaway drogon client keeps a loop thread
// of its own, and a statement lambda it queues holds the connection's last
// reference -- released while that lambda is still queued, ~Sqlite3Connection
// runs on that very thread and ~EventLoopThread joins the thread it is running
// on: EDEADLK, SIGABRT with no assertion reported.
using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

// Throws on failure, as the client calls these replace did: this suite's
// assertion count is fixed, and doctest reports an escaping exception either
// way.
DbHandle openFile(const char* path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path, &raw) != SQLITE_OK) {
    const std::string error = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error("cannot open " + std::string(path) + ": " + error);
  }
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message + " <- " + sql);
  }
  sqlite3_free(error);
}

void seedIdentityDb(const char* path)
{
  std::remove(path);
  const auto db = openFile(path);
  exec(db.get(),
       "CREATE TABLE user ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "name TEXT NOT NULL, last_name TEXT NOT NULL DEFAULT '', "
       "role TEXT NOT NULL CHECK (role IN ('owner', 'resident', 'guard', "
       "'guest')), lang TEXT NOT NULL DEFAULT 'es' "
       "CHECK (lang IN ('es', 'en')), "
       "is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)), "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
       "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(), "INSERT INTO user (id, name, role, is_active) "
                 "VALUES (1, 'Owner', 'owner', 1)");
  exec(db.get(),
       "CREATE TABLE refresh_token ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
       "access_token TEXT NOT NULL, refresh_token TEXT NOT NULL, "
       "device_hash TEXT NOT NULL, user_agent TEXT NOT NULL DEFAULT '', "
       "is_valid INTEGER NOT NULL DEFAULT 1, is_used INTEGER NOT NULL DEFAULT 0, "
       "expires_at INTEGER NOT NULL, "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  exec(db.get(),
       "CREATE TABLE device_credential ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE, "
       "device_hash TEXT NOT NULL, secret_hash TEXT NOT NULL UNIQUE, "
       "is_active INTEGER NOT NULL DEFAULT 1, "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  // sha256Hex is lowercase hex, so the literal carries nothing to escape.
  exec(db.get(),
       "INSERT INTO device_credential (user_id, device_hash, secret_hash) "
       "VALUES (1, '', '" +
           DeviceFilter::sha256Hex(kSecret) + "')");
  exec(db.get(),
       "CREATE TABLE device_login_challenge ("
       "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
       "challenge_id TEXT NOT NULL UNIQUE, device_hash TEXT NOT NULL, "
       "user_agent TEXT NOT NULL DEFAULT '', "
       "status TEXT NOT NULL DEFAULT 'pending', user_id INTEGER, "
       "access_token TEXT, refresh_token TEXT, expires_at INTEGER NOT NULL, "
       "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
}

// Runs the app and stops it however the case body leaves. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
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

// Hosts the real service over the seeded database and points identity.target at it.
class IdentityRpcHarness
{
public:
  explicit IdentityRpcHarness(std::string fleetSecret = {})
      : service_(nullptr, std::move(fleetSecret))
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ConfigService::setRuntimeString("identity.target",
                                    "127.0.0.1:" + std::to_string(port));
  }

  ~IdentityRpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  bool listening() const { return server_ != nullptr; }

private:
  IdentityRpcService service_;
  std::unique_ptr<grpc::Server> server_;
};

bool hexShape(const std::string& value, size_t length)
{
  if (value.size() != length)
    return false;
  for (char c : value) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex)
      return false;
  }
  return true;
}

} // namespace

TEST_CASE("device credential fingerprints are pinned and IP-free")
{
  setConfig();

  const auto secretHash = DeviceFilter::sha256Hex(kSecret);
  CHECK(secretHash ==
        "2a8abfa8cb9906290437854193ca6bca41d4d4e26d1d454bd66a35158095e737");
  CHECK(DeviceFilter::credentialFingerprint(kUa, secretHash) ==
        "5b7ed91198d33d7982e10c5bcba905637d88022b5f2c5b85dd9f97b75fbacaaf");

  DeviceFilter ipFilter;
  auto lan = drogon::HttpRequest::newHttpRequest();
  lan->addHeader("User-Agent", kUa);
  setSourceIp(lan, "10.0.0.1");
  lan->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(ipFilter.doFilter(lan));
  CHECK(deviceCtx(lan).deviceHash ==
        "1975e81a234fd02f4ae788a8fdb0911b1a1f15dd6d5d6d21d311fe4bbe130ceb");

  auto other = drogon::HttpRequest::newHttpRequest();
  other->addHeader("User-Agent", kUa);
  setSourceIp(other, "10.0.0.2");
  other->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(ipFilter.doFilter(other));
  CHECK(deviceCtx(other).deviceHash ==
        "df791307570263b5c3e0d2890a8836027b975be9296209519d2fc8ba341ac4ab");
}

TEST_CASE("credential identity mode issues, binds and authenticates devices")
{
  setConfig();
  const TempDb db("device-credential-test");
  seedIdentityDb(db.path().c_str());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  IdentityRpcHarness identityRpc;
  REQUIRE(identityRpc.listening());

  JwtFilter jwtFilter;
  DeviceFilter deviceFilter;
  AuthService authService;
  const int64_t now = std::time(nullptr);

  ConfigService::setRuntimeString("device.identity_mode", "credential");
  auto first = drogon::HttpRequest::newHttpRequest();
  first->addHeader("User-Agent", kUa);
  setSourceIp(first, "10.0.0.1");
  first->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(deviceFilter.doFilter(first));
  CHECK(deviceCtx(first).deviceHash ==
        "5b7ed91198d33d7982e10c5bcba905637d88022b5f2c5b85dd9f97b75fbacaaf");

  auto second = drogon::HttpRequest::newHttpRequest();
  second->addHeader("User-Agent", kUa);
  setSourceIp(second, "10.0.0.2");
  second->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(deviceFilter.doFilter(second));
  CHECK(deviceCtx(second).deviceHash == deviceCtx(first).deviceHash);

  auto unknown = drogon::HttpRequest::newHttpRequest();
  unknown->addHeader("User-Agent", kUa);
  unknown->addHeader("X-Argus-Device-Credential", "ff00ff00ff00ff00");
  drogon::sync_wait(deviceFilter.doFilter(unknown));
  CHECK(deviceCtx(unknown).deviceHash.empty());

  auto missing = drogon::HttpRequest::newHttpRequest();
  missing->addHeader("User-Agent", kUa);
  drogon::sync_wait(deviceFilter.doFilter(missing));
  CHECK(deviceCtx(missing).deviceHash.empty());

  auto oversized = drogon::HttpRequest::newHttpRequest();
  oversized->addHeader("User-Agent", kUa);
  oversized->addHeader("X-Argus-Device-Credential", std::string(512, 'a'));
  drogon::sync_wait(deviceFilter.doFilter(oversized));
  CHECK(deviceCtx(oversized).deviceHash.empty());

  const auto seededToken = JwtService().generateAccess({{"sub", "1"}});
  auto client = DbService::client();
  client->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) "
      "VALUES (1, ?, 'seed-refresh', "
      "'1975e81a234fd02f4ae788a8fdb0911b1a1f15dd6d5d6d21d311fe4bbe130ceb', ?, "
      "?)",
      seededToken, kUa, now + 3600);
  auto rejected = drogon::HttpRequest::newHttpRequest();
  rejected->addHeader("User-Agent", kUa);
  rejected->addHeader("Authorization", "Bearer " + seededToken);
  drogon::sync_wait(deviceFilter.doFilter(rejected));
  CHECK(deviceCtx(rejected).deviceHash.empty());
  const auto mismatch = refusalOf(jwtFilter.doFilter(rejected));
  REQUIRE(mismatch);
  CHECK(mismatch->status == 401);
  CHECK(mismatch->code == "UNAUTHORIZED");
  CHECK(mismatch->message == "Device mismatch");

  auto boundToEmpty = drogon::HttpRequest::newHttpRequest();
  boundToEmpty->addHeader("User-Agent", kUa);
  boundToEmpty->addHeader("Authorization", "Bearer " + seededToken);
  boundToEmpty->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(deviceFilter.doFilter(boundToEmpty));
  CHECK_FALSE(deviceCtx(boundToEmpty).deviceHash.empty());
  const auto stillMismatch = refusalOf(jwtFilter.doFilter(boundToEmpty));
  REQUIRE(stillMismatch);
  CHECK(stillMismatch->status == 401);
  CHECK(stillMismatch->message == "Device mismatch");
  client->execSqlSync("DELETE FROM refresh_token");

  client->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) "
      "VALUES (1, ?, 'seed-refresh-unbound', '', ?, ?)",
      seededToken, kUa, now + 3600);
  auto unboundSession = drogon::HttpRequest::newHttpRequest();
  unboundSession->addHeader("User-Agent", kUa);
  unboundSession->addHeader("Authorization", "Bearer " + seededToken);
  drogon::sync_wait(deviceFilter.doFilter(unboundSession));
  CHECK(deviceCtx(unboundSession).deviceHash.empty());
  const auto admitted = drogon::sync_wait(jwtFilter.doFilter(unboundSession));
  CHECK_FALSE(admitted);
  client->execSqlSync("DELETE FROM refresh_token");

  const auto created = drogon::sync_wait(
      authService.createDeviceLogin({.deviceHash = "", .userAgent = kDesktopUa}));
  REQUIRE(hexShape(created.challengeId, 64));
  const auto approved =
      drogon::sync_wait(authService.approveDeviceLogin(created.challengeId, 1));
  CHECK(approved);

  const auto polled =
      drogon::sync_wait(authService.pollDeviceLogin(created.challengeId));
  CHECK(polled.status == "approved");
  CHECK(hexShape(polled.deviceSecret, 64));

  auto desktop = drogon::HttpRequest::newHttpRequest();
  desktop->addHeader("User-Agent", kDesktopUa);
  desktop->addHeader("X-Argus-Device-Credential", polled.deviceSecret);
  drogon::sync_wait(deviceFilter.doFilter(desktop));
  const auto expectedHash = DeviceFilter::credentialFingerprint(
      kDesktopUa, DeviceFilter::sha256Hex(polled.deviceSecret));
  CHECK(deviceCtx(desktop).deviceHash == expectedHash);

  desktop->addHeader("Authorization", "Bearer " + polled.accessToken);
  const auto authenticated = drogon::sync_wait(jwtFilter.doFilter(desktop));
  CHECK_FALSE(authenticated);
  CHECK(desktop->getAttributes()
            ->get<JwtContext>(AuthContext::kJwtKey)
            .sub == 1);

  const auto replayed =
      drogon::sync_wait(authService.pollDeviceLogin(created.challengeId));
  CHECK(replayed.status == "expired");
  CHECK(replayed.deviceSecret.empty());

  ConfigService::setRuntimeString("device.identity_mode", "");
  const auto ipChallenge = drogon::sync_wait(authService.createDeviceLogin(
      {.deviceHash =
           "1975e81a234fd02f4ae788a8fdb0911b1a1f15dd6d5d6d21d311fe4bbe130ceb",
       .userAgent = kDesktopUa}));
  REQUIRE(drogon::sync_wait(
      authService.approveDeviceLogin(ipChallenge.challengeId, 1)));
  const auto ipPolled =
      drogon::sync_wait(authService.pollDeviceLogin(ipChallenge.challengeId));
  CHECK(ipPolled.status == "approved");
  CHECK(ipPolled.deviceSecret.empty());
  CHECK_FALSE(ipPolled.toJson().isMember("device_secret"));

  const auto ipRows = client->execSqlSync(
      "SELECT device_hash FROM refresh_token ORDER BY id");
  REQUIRE(ipRows.size() == 2);
  CHECK(ipRows.front()["device_hash"].as<std::string>() ==
        deviceCtx(desktop).deviceHash);
  CHECK(ipRows.back()["device_hash"].as<std::string>() ==
        "1975e81a234fd02f4ae788a8fdb0911b1a1f15dd6d5d6d21d311fe4bbe130ceb");

  // A/B parity: the RPC path's JwtContext equals the direct repository read.
  const auto& rpcCtx =
      desktop->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto directUser = drogon::sync_wait(UserRepository().findById(1));
  REQUIRE(directUser);
  const auto directRt = drogon::sync_wait(
      RefreshTokenRepository().findByAccessToken(1, polled.accessToken));
  REQUIRE(directRt);
  CHECK(rpcCtx.sub == directUser->id);
  CHECK(rpcCtx.name == directUser->name + " " + directUser->lastName);
  CHECK(rpcCtx.role == directUser->role);
  CHECK(rpcCtx.isActive == directUser->isActive);
  CHECK(rpcCtx.deviceHash == directRt->deviceHash);

  // Fail closed: an unreachable identity service rejects, it does not admit.
  ConfigService::setRuntimeString("identity.target", "127.0.0.1:1");
  auto unreachable = drogon::HttpRequest::newHttpRequest();
  unreachable->addHeader("User-Agent", kDesktopUa);
  unreachable->addHeader("Authorization", "Bearer " + polled.accessToken);
  const auto refused = refusalOf(jwtFilter.doFilter(unreachable));
  REQUIRE(refused);
  CHECK(refused->status == 401);
  CHECK(refused->message == "Authentication required");

  // The same request that authenticates with the secret is refused without it.
  {
    IdentityRpcHarness guarded(kFleetSecret);
    REQUIRE(guarded.listening());
    ConfigService::setRuntimeString("device.identity_mode", "credential");

    ConfigService::setRuntimeString("identity.rpc_secret", "");
    auto noSecret = drogon::HttpRequest::newHttpRequest();
    noSecret->addHeader("User-Agent", kDesktopUa);
    noSecret->addHeader("X-Argus-Device-Credential", polled.deviceSecret);
    noSecret->addHeader("Authorization", "Bearer " + polled.accessToken);
    drogon::sync_wait(deviceFilter.doFilter(noSecret));
    CHECK(deviceCtx(noSecret).deviceHash.empty());
    const auto rejectedCall = refusalOf(jwtFilter.doFilter(noSecret));
    REQUIRE(rejectedCall);
    CHECK(rejectedCall->status == 401);
    CHECK(rejectedCall->message == "Authentication required");

    ConfigService::setRuntimeString("identity.rpc_secret", kFleetSecret);
    auto withSecret = drogon::HttpRequest::newHttpRequest();
    withSecret->addHeader("User-Agent", kDesktopUa);
    withSecret->addHeader("X-Argus-Device-Credential", polled.deviceSecret);
    withSecret->addHeader("Authorization", "Bearer " + polled.accessToken);
    drogon::sync_wait(deviceFilter.doFilter(withSecret));
    CHECK(deviceCtx(withSecret).deviceHash == expectedHash);
    const auto admittedCall = drogon::sync_wait(jwtFilter.doFilter(withSecret));
    CHECK_FALSE(admittedCall);
    ConfigService::setRuntimeString("identity.rpc_secret", "");
  }
}
