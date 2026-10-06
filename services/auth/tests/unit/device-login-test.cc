#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <app/rpc/auth-callers.hxx>
#include <app/rpc/auth-rpc-service.hxx>
#include <app/rpc/local-auth-client.hxx>
#include <auth/auth-access.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <auth/device-filter.hxx>
#include <auth/device-login-status.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/jwt-service.hxx>
#include <auth/request-context.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/auth/dtos/start-device-login-dto.hxx>
#include <feature/auth/services/auth-feature-service.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/device/repositories/device-login-challenge/device-login-challenge-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#ifndef ARGUS_AUTH_SCHEMA_PATH
#error "ARGUS_AUTH_SCHEMA_PATH must point at the auth schema.sql"
#endif

namespace
{

constexpr const char* kJwtSecret =
    "f5-2-device-credential-test-jwt-secret-0123456789";
constexpr const char* kRefreshSecret =
    "f5-2-device-credential-test-refresh-secret-0123456789";
constexpr const char* kFingerprintSecret =
    "f5-2-test-fingerprint-secret-0123456789";
constexpr const char* kUa = "argus-ua/1.0";
constexpr const char* kDesktopUa = "argus-desktop/1.0";
constexpr const char* kSecret = "00112233445566778899aabbccddeeff"
                                "00112233445566778899aabbccddeeff";
constexpr const char* kFleetSecret = "f7-r-fleet-secret-0123456789abcdef";
constexpr const char* kIpFingerprint =
    "39bfb6d938f1b0d706d2bb1bbe73246dfd7bb5ea26e6d2809ceea33b761b510e";
constexpr int64_t kUserId = 1;

struct Refusal
{
  int status;
  std::string code;
  std::string message;
};

template <typename T>
[[nodiscard]] std::optional<Refusal> refusalOf(drogon::Task<T> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{.status = error.statusCode(),
                   .code = error.errorCode(),
                   .message = std::string(error.what())};
  }
  return std::nullopt;
}

[[nodiscard]] int tempCounter()
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

void setConfig()
{
  ConfigService::setRuntimeString("jwt.secret", kJwtSecret);
  ConfigService::setRuntimeString("jwt.refresh_secret", kRefreshSecret);
  ConfigService::setRuntimeString("jwt.access_ttl_minutes", "60");
  ConfigService::setRuntimeString("jwt.refresh_ttl_days", "7");
  ConfigService::setRuntimeString("device.fingerprint_secret",
                                  kFingerprintSecret);
  ConfigService::setRuntimeString("device.identity_mode", "ip");
  ConfigService::setRuntimeString("device.trust_forwarded_for", "true");
  ConfigService::setRuntimeString(
      "device.trusted_proxy_ips",
      drogon::HttpRequest::newHttpRequest()->getPeerAddr().toIp());
}

void setSourceIp(const drogon::HttpRequestPtr& req, const std::string& ip)
{
  req->addHeader("X-Forwarded-For", ip);
}

[[nodiscard]] const DeviceContext& deviceCtx(const drogon::HttpRequestPtr& req)
{
  return req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
}

[[nodiscard]] const JwtContext& jwtCtx(const drogon::HttpRequestPtr& req)
{
  return req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
}

[[nodiscard]] bool hexShape(const std::string& value, std::size_t length)
{
  if (value.size() != length)
    return false;
  for (const char c : value) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex)
      return false;
  }
  return true;
}

class ScriptedIdentityClient : public IdentityClient
{
public:
  ScriptedIdentityClient() : IdentityClient("127.0.0.1:1") {}

  [[nodiscard]] std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const override
  {
    if (beforeGetUser) {
      auto hook = std::move(beforeGetUser);
      beforeGetUser = nullptr;
      hook();
    }
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

  bool reachable{true};
  bool isActive{true};
  mutable std::function<void()> beforeGetUser;
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
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
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

  TempDb db_{"auth-device-login-test"};
  ScriptedIdentityClient identity_;
  std::thread runner_;
  bool started_{false};
  bool ready_{false};
};

[[nodiscard]] std::unique_ptr<Fixture>& fixtureStorage()
{
  static std::unique_ptr<Fixture> booted;
  return booted;
}

[[nodiscard]] Fixture& fixture()
{
  auto& booted = fixtureStorage();
  if (!booted)
    booted = std::make_unique<Fixture>();
  return *booted;
}

void stopFixture()
{
  fixtureStorage().reset();
}

void seedCredential(const std::string& secretHash)
{
  DbService::client()->execSqlSync(
      "INSERT INTO device_credential (user_id, device_hash, secret_hash, "
      "is_active) VALUES (?, '', ?, 1)",
      kUserId, secretHash);
}

struct AuthRpcHarnessInput
{
  SessionService* sessions{nullptr};
  DeviceCredentialRepository* credentials{nullptr};
  std::string fleetSecret;
};

class AuthRpcHarness
{
public:
  explicit AuthRpcHarness(AuthRpcHarnessInput input)
      : service_({.sessions = input.sessions,
                  .deviceCredentials = input.credentials},
                 std::move(input.fleetSecret))
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    target_ = "127.0.0.1:" + std::to_string(port);
  }

  ~AuthRpcHarness()
  {
    if (server_)
      server_->Shutdown();
  }

  AuthRpcHarness(const AuthRpcHarness&) = delete;
  AuthRpcHarness& operator=(const AuthRpcHarness&) = delete;

  [[nodiscard]] bool listening() const { return server_ != nullptr; }

  [[nodiscard]] const std::string& target() const { return target_; }

private:
  AuthRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

}

TEST_CASE("device credential fingerprints are pinned and IP-free")
{
  setConfig();

  const auto secretHash = DeviceFilter::sha256Hex(kSecret);
  CHECK(secretHash ==
        "2a8abfa8cb9906290437854193ca6bca41d4d4e26d1d454bd66a35158095e737");
  CHECK(DeviceFilter::credentialFingerprint(kUa, secretHash) ==
        "32eeeefe03810d4edcd554f75b16b5403db30a305aa9dcd4a60f4d51db1eb712");

  DeviceFilter ipFilter;
  auto lan = drogon::HttpRequest::newHttpRequest();
  lan->addHeader("User-Agent", kUa);
  setSourceIp(lan, "10.0.0.1");
  lan->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(ipFilter.doFilter(lan));
  CHECK(deviceCtx(lan).deviceHash == kIpFingerprint);

  auto other = drogon::HttpRequest::newHttpRequest();
  other->addHeader("User-Agent", kUa);
  setSourceIp(other, "10.0.0.2");
  other->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(ipFilter.doFilter(other));
  CHECK(deviceCtx(other).deviceHash ==
        "062bed41f23486680f6fbbefe49d39863def73325068b1bf919601f659788860");
}

TEST_CASE("credential identity mode issues, binds and authenticates devices")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness rpc({.sessions = &sessions,
                      .credentials = &credentials,
                      .fleetSecret = ""});
  REQUIRE(rpc.listening());
  ConfigService::setRuntimeString("auth.target", rpc.target());

  seedCredential(DeviceFilter::sha256Hex(kSecret));

  AuthFeatureService authService(
      {.jwtService = JwtService{JwtRole::Issuer},
       .refreshTokenRepository = RefreshTokenRepository{},
       .deviceCredentialRepository = DeviceCredentialRepository{},
       .challengeRepository = DeviceLoginChallengeRepository{},
       .sessions = SessionManagementService(
           {.refreshTokenRepository = RefreshTokenRepository{}}),
       .identity = &app.identity()},
      AuthFeatureService::Config{.refreshReuseGraceSeconds = 30, .allowRemoteQrLogin = false});

  JwtFilter jwtFilter;
  DeviceFilter deviceFilter;
  const int64_t now = std::time(nullptr);

  ConfigService::setRuntimeString("device.identity_mode", "credential");

  auto first = drogon::HttpRequest::newHttpRequest();
  first->addHeader("User-Agent", kUa);
  setSourceIp(first, "10.0.0.1");
  first->addHeader("X-Argus-Device-Credential", kSecret);
  drogon::sync_wait(deviceFilter.doFilter(first));
  CHECK(deviceCtx(first).deviceHash ==
        "32eeeefe03810d4edcd554f75b16b5403db30a305aa9dcd4a60f4d51db1eb712");

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

  const auto seededToken = JwtService(JwtRole::Issuer).generateAccess({{"sub", "1"}});
  auto client = DbService::client();
  client->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) VALUES (1, ?, 'seed-refresh', ?, ?, "
      "?)",
      seededToken, kIpFingerprint, kUa, now + 3600);
  auto rejected = drogon::HttpRequest::newHttpRequest();
  rejected->addHeader("User-Agent", kUa);
  rejected->addHeader("Authorization", "Bearer " + seededToken);
  drogon::sync_wait(deviceFilter.doFilter(rejected));
  CHECK(deviceCtx(rejected).deviceHash.empty());
  const auto mismatch = refusalOf(jwtFilter.doFilter(rejected));
  if (!mismatch.has_value()) {
    FAIL("the session bound to another fingerprint was not refused");
    return;
  }
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
  if (!stillMismatch.has_value()) {
    FAIL("the credential did not mismatch the IP-bound session");
    return;
  }
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

  const std::string loginProof(64, 'b');
  const auto created = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .pollHash = DeviceFilter::sha256Hex(loginProof),
       .origin = SessionOrigin::Lan,
       .ipAddress = "10.0.0.7"}));
  REQUIRE(hexShape(created.challengeId, 64));
  REQUIRE_NOTHROW(
      drogon::sync_wait(authService.approveDeviceLogin(created.challengeId, 1)));

  const auto watcher = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = created.challengeId,
       .device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = ""}));
  CHECK(watcher.status == DeviceLoginStatus::Pending);
  CHECK(watcher.accessToken.empty());
  const auto guesser = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = created.challengeId,
       .device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = std::string(64, 'c')}));
  CHECK(guesser.status == DeviceLoginStatus::Pending);
  CHECK(guesser.accessToken.empty());

  const auto polled = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = created.challengeId,
       .device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = loginProof}));
  CHECK(polled.status == DeviceLoginStatus::Approved);
  CHECK(polled.userId == kUserId);
  CHECK(polled.name == "Ada Rico");
  CHECK(polled.role == UserRole::Owner);
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
  CHECK(jwtCtx(desktop).sub == kUserId);
  CHECK(jwtCtx(desktop).name == "Ada Rico");
  CHECK(jwtCtx(desktop).role == UserRole::Owner);
  CHECK(jwtCtx(desktop).isActive);
  CHECK(jwtCtx(desktop).deviceHash == expectedHash);

  const auto contended = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .pollHash = DeviceFilter::sha256Hex(loginProof),
       .origin = SessionOrigin::Lan,
       .ipAddress = "10.0.0.7"}));
  REQUIRE(hexShape(contended.challengeId, 64));
  app.identity().beforeGetUser = [&client, &contended] {
    client->execSqlSync(
        "UPDATE device_login_challenge SET status = ? WHERE challenge_id = ?",
        deviceLoginStatusToString(DeviceLoginStatus::Approved),
        contended.challengeId);
  };
  const auto lost =
      refusalOf(authService.approveDeviceLogin(contended.challengeId, 1));
  if (!lost.has_value()) {
    FAIL("the approve that lost the challenge race was not refused");
    return;
  }
  CHECK(lost->status == 404);
  CHECK(lost->code == "NOT_FOUND");
  CHECK(lost->message == "Challenge not found");

  const auto replayed = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = created.challengeId,
       .device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = loginProof}));
  CHECK(replayed.status == DeviceLoginStatus::Expired);
  CHECK(replayed.deviceSecret.empty());

  ConfigService::setRuntimeString("device.identity_mode", "ip");
  const auto ipChallenge = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .pollHash = DeviceFilter::sha256Hex(loginProof),
       .origin = SessionOrigin::Lan,
       .ipAddress = "10.0.0.1"}));
  REQUIRE_NOTHROW(drogon::sync_wait(
      authService.approveDeviceLogin(ipChallenge.challengeId, 1)));
  const auto snooped = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = ipChallenge.challengeId,
       .device = {.deviceHash = "another-device", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = ""}));
  CHECK(snooped.status == DeviceLoginStatus::Pending);
  CHECK(snooped.accessToken.empty());
  const auto ipPolled = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = ipChallenge.challengeId,
       .device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = loginProof}));
  CHECK(ipPolled.status == DeviceLoginStatus::Approved);
  CHECK(ipPolled.deviceSecret.empty());
  CHECK_FALSE(ipPolled.toJson().isMember("device_secret"));

  const auto liveRows = client->execSqlSync(
      "SELECT device_hash FROM refresh_token ORDER BY id");
  REQUIRE(liveRows.size() == 2);
  CHECK(liveRows.front()["device_hash"].as<std::string>() == expectedHash);
  CHECK(liveRows.back()["device_hash"].as<std::string>() == kIpFingerprint);

  const auto bound = drogon::sync_wait(
      RefreshTokenRepository().findByAccessToken(kUserId, polled.accessToken));
  if (!bound.has_value()) {
    FAIL("the approved session row was not persisted");
    return;
  }
  CHECK(bound->deviceHash == jwtCtx(desktop).deviceHash);

  const auto claimed = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .pollHash = DeviceFilter::sha256Hex(loginProof),
       .origin = SessionOrigin::Lan,
       .ipAddress = "10.0.0.1"}));
  REQUIRE_NOTHROW(drogon::sync_wait(authService.approveDeviceLogin(claimed.challengeId, 1)));
  const DeviceLoginChallengeRepository challenges;
  const auto claimAt = static_cast<int64_t>(std::time(nullptr));
  CHECK(drogon::sync_wait(challenges.claimApproved(claimed.challengeId, claimAt)));
  CHECK_FALSE(drogon::sync_wait(challenges.claimApproved(claimed.challengeId, claimAt)));
  const auto secondPoller = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = claimed.challengeId,
       .device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = loginProof}));
  CHECK(secondPoller.status != DeviceLoginStatus::Approved);
  CHECK(secondPoller.accessToken.empty());

  const auto refusedApproval = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .pollHash = DeviceFilter::sha256Hex(loginProof),
       .origin = SessionOrigin::Lan,
       .ipAddress = "10.0.0.1"}));
  app.identity().reachable = false;
  const auto identityDown =
      refusalOf(authService.approveDeviceLogin(refusedApproval.challengeId, 1));
  if (!identityDown.has_value()) {
    FAIL("the approve was not refused while identity was down");
    return;
  }
  CHECK(identityDown->status == 503);
  app.identity().reachable = true;
  app.identity().isActive = false;
  const auto inactiveApprover =
      refusalOf(authService.approveDeviceLogin(refusedApproval.challengeId, 1));
  if (!inactiveApprover.has_value()) {
    FAIL("the approve of an inactive approver was not refused");
    return;
  }
  CHECK(inactiveApprover->status == 403);
  app.identity().isActive = true;

  auto unbound = drogon::HttpRequest::newHttpRequest();
  unbound->addHeader("User-Agent", kDesktopUa);
  unbound->addHeader("Authorization", "Bearer " + polled.accessToken);
  const auto noDevice = refusalOf(jwtFilter.doFilter(unbound));
  if (!noDevice.has_value()) {
    FAIL("a request that skipped the device filter was admitted");
    return;
  }
  CHECK(noDevice->status == 500);
  CHECK(noDevice->message == "The route binds no device to the session");

  ConfigService::setRuntimeString("auth.target", "127.0.0.1:1");
  auto unreachable = drogon::HttpRequest::newHttpRequest();
  unreachable->addHeader("User-Agent", kDesktopUa);
  unreachable->addHeader("Authorization", "Bearer " + polled.accessToken);
  drogon::sync_wait(deviceFilter.doFilter(unreachable));
  const auto refused = refusalOf(jwtFilter.doFilter(unreachable));
  if (!refused.has_value()) {
    FAIL("an unreachable auth RPC did not refuse the verdict");
    return;
  }
  CHECK(refused->status == 503);
  CHECK(refused->message == "The auth service is unavailable");

  {
    AuthRpcHarness guarded({.sessions = &sessions,
                            .credentials = &credentials,
                            .fleetSecret = kFleetSecret});
    REQUIRE(guarded.listening());
    ConfigService::setRuntimeString("auth.target", guarded.target());
    ConfigService::setRuntimeString("device.identity_mode", "credential");

    ConfigService::setRuntimeString("auth.rpc_secret", "");
    auto noSecret = drogon::HttpRequest::newHttpRequest();
    noSecret->addHeader("User-Agent", kDesktopUa);
    noSecret->addHeader("X-Argus-Device-Credential", polled.deviceSecret);
    noSecret->addHeader("Authorization", "Bearer " + polled.accessToken);
    const auto rejectedCall = refusalOf(deviceFilter.doFilter(noSecret));
    if (!rejectedCall.has_value()) {
      FAIL("the fleet-secret gate did not refuse an unqualified caller");
      return;
    }
    CHECK(rejectedCall->status == 503);
    CHECK(rejectedCall->message == "The auth service is unavailable");

    ConfigService::setRuntimeString("auth.rpc_secret", kFleetSecret);
    auto withSecret = drogon::HttpRequest::newHttpRequest();
    withSecret->addHeader("User-Agent", kDesktopUa);
    withSecret->addHeader("X-Argus-Device-Credential", polled.deviceSecret);
    withSecret->addHeader("Authorization", "Bearer " + polled.accessToken);
    drogon::sync_wait(deviceFilter.doFilter(withSecret));
    CHECK(deviceCtx(withSecret).deviceHash == expectedHash);
    const auto admittedCall = drogon::sync_wait(jwtFilter.doFilter(withSecret));
    CHECK_FALSE(admittedCall);
    CHECK(jwtCtx(withSecret).sub == kUserId);
    ConfigService::setRuntimeString("auth.rpc_secret", "");
    ConfigService::setRuntimeString("device.identity_mode", "ip");
  }
}

TEST_CASE("a refresh keeps the session on the device and agent it was issued to")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  AuthFeatureService authService(
      {.jwtService = JwtService{JwtRole::Issuer},
       .refreshTokenRepository = RefreshTokenRepository{},
       .deviceCredentialRepository = DeviceCredentialRepository{},
       .challengeRepository = DeviceLoginChallengeRepository{},
       .sessions = SessionManagementService(
           {.refreshTokenRepository = RefreshTokenRepository{}}),
       .identity = &app.identity()},
      AuthFeatureService::Config{.refreshReuseGraceSeconds = 30, .allowRemoteQrLogin = false});

  const std::string boundHash = DeviceFilter::credentialFingerprint(
      kUa, DeviceFilter::sha256Hex(kSecret));
  const auto seed = [&](const std::string& refresh) {
    DbService::client()->execSqlSync(
        "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
        "device_hash, user_agent, expires_at) VALUES (1, ?, ?, ?, ?, ?)",
        JwtService(JwtRole::Issuer).generateAccess({{"sub", "1"}}), refresh, boundHash, kUa,
        static_cast<int64_t>(std::time(nullptr)) + 3600);
  };
  const auto refreshFrom = [&](const RefreshTokenInput& input) {
    return refusalOf(authService.refreshToken(input));
  };

  ConfigService::setRuntimeString("device.identity_mode", "credential");

  const auto stolen = JwtService(JwtRole::Issuer).generateRefresh({{"sub", "1"}});
  seed(stolen);
  const auto noCredential = refreshFrom(
      {.body = {.refreshToken = stolen},
       .deviceHash = "",
       .userAgent = kUa,
       .ip = "10.0.0.1",
       .credentialHash = "",
       .client = {},
       .networkHash = ""});
  CHECK((noCredential.has_value() && noCredential->status == 401));

  const auto noAgent = refreshFrom(
      {.body = {.refreshToken = stolen},
       .deviceHash = boundHash,
       .userAgent = "",
       .ip = "10.0.0.1",
       .credentialHash = "",
       .client = {},
       .networkHash = ""});
  CHECK((noAgent.has_value() && noAgent->status == 401));

  CHECK_FALSE(refreshFrom({.body = {.refreshToken = stolen},
                           .deviceHash = boundHash,
                           .userAgent = kUa,
                           .ip = "10.0.0.1",
                           .credentialHash = "",
                           .client = {},
                           .networkHash = ""})
                  .has_value());

  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  ConfigService::setRuntimeString("device.identity_mode", "ip");
  const auto roaming = JwtService(JwtRole::Issuer).generateRefresh({{"sub", "1"}});
  seed(roaming);
  CHECK(refreshFrom({.body = {.refreshToken = roaming},
                           .deviceHash = "another-network",
                           .userAgent = kUa,
                           .ip = "10.0.0.2",
                           .credentialHash = "",
                           .client = {},
                           .networkHash = ""})
                  .has_value());

  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  ConfigService::setRuntimeString("device.identity_mode", "ip");
}

TEST_CASE("a QR challenge needs a poll proof, shows where it came from and refuses the tunnel unless opted in")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const auto serviceWith = [&app](bool allowRemoteQrLogin) {
    return AuthFeatureService(
        {.jwtService = JwtService{JwtRole::Issuer},
         .refreshTokenRepository = RefreshTokenRepository{},
         .deviceCredentialRepository = DeviceCredentialRepository{},
         .challengeRepository = DeviceLoginChallengeRepository{},
         .sessions = SessionManagementService(
             {.refreshTokenRepository = RefreshTokenRepository{}}),
         .identity = &app.identity()},
        AuthFeatureService::Config{.refreshReuseGraceSeconds = 30,
                                   .allowRemoteQrLogin = allowRemoteQrLogin});
  };
  const AuthFeatureService closed = serviceWith(false);
  const AuthFeatureService open = serviceWith(true);
  const std::string proof(64, 'd');
  const auto startFrom = [&proof](SessionOrigin origin, const std::string& pollHash) {
    return DeviceLoginStartInput{
        .device = {.deviceHash = kIpFingerprint,
                   .userAgent = kDesktopUa,
                   .client = {.platform = SessionPlatform::Desktop, .deviceName = "Desk"},
                   .networkHash = ""},
        .pollHash = pollHash.empty() ? pollHash : DeviceFilter::sha256Hex(proof),
        .origin = origin,
        .ipAddress = "203.0.113.9"};
  };

  Json::Value withoutHash(Json::objectValue);
  Json::Value emptyHash(Json::objectValue);
  emptyHash["pollHash"] = "";
  Json::Value upperHash(Json::objectValue);
  upperHash["pollHash"] = std::string(64, 'A');
  for (const auto& body : {withoutHash, emptyHash, upperHash}) {
    const auto request = drogon::HttpRequest::newHttpJsonRequest(body);
    try {
      static_cast<void>(StartDeviceLoginDto::fromRequest(request));
      FAIL("a QR challenge without a well-formed poll hash must be refused");
    }
    catch (const ValidationException& refusal) {
      CHECK(refusal.statusCode() == 422);
    }
  }

  const auto tunnelled =
      refusalOf(closed.createDeviceLogin(startFrom(SessionOrigin::Tunnel, "x")));
  if (!tunnelled.has_value()) {
    FAIL("a tunnelled QR challenge on a closed instance was not refused");
    return;
  }
  CHECK(tunnelled->status == 403);
  CHECK(tunnelled->code == "REMOTE_NOT_ALLOWED");

  const auto remote =
      drogon::sync_wait(open.createDeviceLogin(startFrom(SessionOrigin::Tunnel, "x")));
  const auto details = drogon::sync_wait(open.deviceLoginDetails(remote.challengeId));
  CHECK(details.challengeId == remote.challengeId);
  CHECK(details.origin == SessionOrigin::Tunnel);
  CHECK(details.ipAddress == "203.0.113.9");
  CHECK(details.platform == SessionPlatform::Desktop);
  CHECK(details.deviceName == "Desk");
  CHECK(details.expiresAt == remote.expiresAt);
  const Json::Value shown = details.toJson();
  CHECK(shown["origin"].asString() == "tunnel");
  CHECK(shown["ipAddress"].asString() == "203.0.113.9");
  CHECK_FALSE(shown.isMember("pollHash"));
  CHECK_FALSE(shown.isMember("deviceHash"));

  const auto unknown = refusalOf(open.deviceLoginDetails(std::string(64, 'e')));
  if (!unknown.has_value()) {
    FAIL("an unknown challenge was not refused");
    return;
  }
  CHECK(unknown->status == 404);

  REQUIRE_NOTHROW(drogon::sync_wait(open.approveDeviceLogin(remote.challengeId, 1)));
  const auto afterApproval = refusalOf(open.deviceLoginDetails(remote.challengeId));
  if (!afterApproval.has_value()) {
    FAIL("an approved challenge still showed its details");
    return;
  }
  CHECK(afterApproval->status == 404);

  const auto stored = DbService::client()->execSqlSync(
      "SELECT poll_hash FROM device_login_challenge WHERE challenge_id = ?",
      remote.challengeId);
  REQUIRE(stored.size() == 1);
  CHECK(stored.front()["poll_hash"].as<std::string>() == DeviceFilter::sha256Hex(proof));

  const auto handed = drogon::sync_wait(open.pollDeviceLogin(
      {.challengeId = remote.challengeId,
       .device = {.deviceHash = "", .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
       .proof = proof}));
  CHECK(handed.status == DeviceLoginStatus::Approved);

  const auto legacy = drogon::sync_wait(open.createDeviceLogin(startFrom(SessionOrigin::Lan, "x")));
  REQUIRE_NOTHROW(drogon::sync_wait(open.approveDeviceLogin(legacy.challengeId, 1)));
  DbService::client()->execSqlSync(
      "UPDATE device_login_challenge SET poll_hash = '' WHERE challenge_id = ?",
      legacy.challengeId);
  for (const std::string& attempt : {std::string{}, proof}) {
    const auto withoutHash = drogon::sync_wait(open.pollDeviceLogin(
        {.challengeId = legacy.challengeId,
         .device = {.deviceHash = kIpFingerprint, .userAgent = kDesktopUa, .client = {}, .networkHash = ""},
         .proof = attempt}));
    CHECK(withoutHash.status == DeviceLoginStatus::Pending);
    CHECK(withoutHash.accessToken.empty());
  }
  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  DbService::client()->execSqlSync("DELETE FROM device_login_challenge");
}

TEST_CASE("in ip mode a refresh rotates only from the network the session was bound to")
{
  Fixture& app = fixture();
  REQUIRE(app.start());
  ConfigService::setRuntimeString("device.identity_mode", "ip");

  AuthFeatureService authService(
      {.jwtService = JwtService{JwtRole::Issuer},
       .refreshTokenRepository = RefreshTokenRepository{},
       .deviceCredentialRepository = DeviceCredentialRepository{},
       .challengeRepository = DeviceLoginChallengeRepository{},
       .sessions = SessionManagementService(
           {.refreshTokenRepository = RefreshTokenRepository{}}),
       .identity = &app.identity()},
      AuthFeatureService::Config{.refreshReuseGraceSeconds = 30,
                                 .allowRemoteQrLogin = false});

  const auto from = [](const std::string& address) {
    return DeviceFilter::networkFingerprint({.origin = SessionOrigin::Lan, .address = address});
  };
  const auto input = [&from](const std::string& token, const std::string& address) {
    return RefreshTokenInput{
        .body = {.refreshToken = token},
        .deviceHash = DeviceFilter::addressFingerprint({.userAgent = kUa, .address = address}),
        .userAgent = kUa,
        .ip = address,
        .credentialHash = "",
        .client = {},
        .networkHash = from(address)};
  };
  CHECK(from("192.168.1.20") == from("192.168.1.200"));
  CHECK(from("192.168.1.20") != from("192.168.2.20"));
  CHECK(DeviceFilter::networkFingerprint({.origin = SessionOrigin::Tunnel, .address = "127.0.0.1"}) !=
        from("127.0.0.1"));

  const auto opened = drogon::sync_wait(authService.createDeviceLogin(
      {.device = {.deviceHash = DeviceFilter::addressFingerprint({.userAgent = kUa, .address = "192.168.1.20"}),
                  .userAgent = kUa,
                  .client = {},
                  .networkHash = from("192.168.1.20")},
       .pollHash = DeviceFilter::sha256Hex(std::string(64, 'f')),
       .origin = SessionOrigin::Lan,
       .ipAddress = "192.168.1.20"}));
  REQUIRE_NOTHROW(drogon::sync_wait(authService.approveDeviceLogin(opened.challengeId, 1)));
  const auto session = drogon::sync_wait(authService.pollDeviceLogin(
      {.challengeId = opened.challengeId,
       .device = {.deviceHash = "", .userAgent = kUa, .client = {}, .networkHash = ""},
       .proof = std::string(64, 'f')}));
  REQUIRE(session.status == DeviceLoginStatus::Approved);

  const auto elsewhere = refusalOf(authService.refreshToken(input(session.refreshToken, "198.51.100.4")));
  if (!elsewhere.has_value()) {
    FAIL("a refresh from another network was not refused");
    return;
  }
  CHECK(elsewhere->status == 401);

  const auto rotated = drogon::sync_wait(authService.refreshToken(input(session.refreshToken, "192.168.1.77")));
  CHECK_FALSE(rotated.refreshToken.empty());
  const auto stored = DbService::client()->execSqlSync(
      "SELECT network_hash FROM refresh_token WHERE is_valid = 1 AND is_used = 0");
  REQUIRE(stored.size() == 1);
  CHECK(stored.front()["network_hash"].as<std::string>() == from("192.168.1.77"));

  const auto legacyToken = JwtService(JwtRole::Issuer).generateRefresh({{"sub", "1"}});
  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  DbService::client()->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) VALUES (1, 'legacy-access', ?, ?, ?, ?)",
      legacyToken, DeviceFilter::addressFingerprint({.userAgent = kUa, .address = "192.168.1.20"}),
      std::string(kUa), static_cast<int64_t>(std::time(nullptr)) + 3600);
  CHECK(refusalOf(authService.refreshToken(input(legacyToken, "192.168.1.21"))).has_value());
  CHECK_FALSE(refusalOf(authService.refreshToken(input(legacyToken, "192.168.1.20"))).has_value());
  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  DbService::client()->execSqlSync("DELETE FROM device_login_challenge");
}

TEST_CASE("a forwarded address is the hop the trusted proxy saw, not the client's claim")
{
  setConfig();
  ConfigService::setRuntimeString("device.identity_mode", "ip");
  const auto keyFor = [](const std::string& forwarded) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("User-Agent", kUa);
    req->addHeader("X-Forwarded-For", forwarded);
    return DeviceFilter::deviceKey(req);
  };
  CHECK(keyFor("6.6.6.6, 10.0.0.9") == keyFor("10.0.0.9"));
  CHECK(keyFor("6.6.6.6, 10.0.0.9") != keyFor("6.6.6.6"));
  ConfigService::setRuntimeString("device.identity_mode", "ip");
}

TEST_CASE("argus-auth verifies its own devices and sessions in process once "
          "every other caller is paired")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  std::vector<std::pair<std::string, std::string>> callers;
  for (const auto& caller : auth_callers::expected())
    callers.emplace_back(caller, caller + "-auth-credential-0123456789abcdef");
  AuthRpcService paired({.sessions = &sessions, .deviceCredentials = &credentials},
                        std::make_shared<const argus::client::FleetCallerGate>(
                            argus::client::FleetGateConfig{
                                .expectedCallers = auth_callers::expected(),
                                .callerPairs = std::move(callers),
                                .legacySecret = kFleetSecret,
                                .onFirstLegacy = {}}));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&paired);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server);
  ConfigService::setRuntimeString("auth.target", "127.0.0.1:" + std::to_string(port));
  ConfigService::setRuntimeString("auth.credential", "");
  ConfigService::setRuntimeString("auth.rpc_secret", kFleetSecret);
  ConfigService::setRuntimeString("device.identity_mode", "credential");

  const std::string ownSecret = "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";
  const std::string secretHash = DeviceFilter::sha256Hex(ownSecret);
  seedCredential(secretHash);
  const auto request = [&ownSecret] {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->addHeader("User-Agent", kUa);
    req->addHeader("X-Argus-Device-Credential", ownSecret);
    return req;
  };

  const auto overRpc = refusalOf(DeviceFilter().doFilter(request()));
  CHECK((overRpc.has_value() && overRpc->status == 503));

  LocalAuthClient::install({.sessions = &sessions,
                            .deviceCredentials = &credentials,
                            .timeout = std::chrono::milliseconds(5000)});

  auto device = request();
  drogon::sync_wait(DeviceFilter().doFilter(device));
  const std::string boundHash = DeviceFilter::credentialFingerprint(kUa, secretHash);
  CHECK(deviceCtx(device).deviceHash == boundHash);

  const auto refresh = JwtService(JwtRole::Issuer).generateRefresh({{"sub", "1"}});
  const auto access = JwtService(JwtRole::Issuer).generateAccess({{"sub", "1"}});
  DbService::client()->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) VALUES (1, ?, ?, ?, ?, ?)",
      access, refresh, boundHash, std::string(kUa),
      static_cast<int64_t>(std::time(nullptr)) + 3600);

  device->addHeader("Authorization", "Bearer " + access);
  CHECK_FALSE(drogon::sync_wait(JwtFilter().doFilter(device)));
  CHECK(jwtCtx(device).sub == kUserId);

  AuthFeatureService authService(
      {.jwtService = JwtService{JwtRole::Issuer},
       .refreshTokenRepository = RefreshTokenRepository{},
       .deviceCredentialRepository = DeviceCredentialRepository{},
       .challengeRepository = DeviceLoginChallengeRepository{},
       .sessions = SessionManagementService(
           {.refreshTokenRepository = RefreshTokenRepository{}}),
       .identity = &app.identity()},
      AuthFeatureService::Config{.refreshReuseGraceSeconds = 30, .allowRemoteQrLogin = false});
  CHECK_FALSE(refusalOf(authService.refreshToken(
                            {.body = {.refreshToken = refresh},
                             .deviceHash = deviceCtx(device).deviceHash,
                             .userAgent = kUa,
                             .ip = "10.0.0.1",
                             .credentialHash = secretHash,
                             .client = {},
                             .networkHash = ""}))
                  .has_value());

  installLocalAuthClient(nullptr);
  server->Shutdown();
  DbService::client()->execSqlSync("DELETE FROM refresh_token");
  DbService::client()->execSqlSync(
      "DELETE FROM device_credential WHERE secret_hash = ?", secretHash);
  ConfigService::setRuntimeString("auth.rpc_secret", "");
  ConfigService::setRuntimeString("device.identity_mode", "ip");
}

int main(int argc, char** argv)
{
  doctest::Context context(argc, argv);
  const int result = context.run();
  stopFixture();
  return result;
}
