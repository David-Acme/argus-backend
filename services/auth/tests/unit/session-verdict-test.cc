#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <app/rpc/auth-rpc-service.hxx>
#include <argus/auth/v1/auth.grpc.pb.h>
#include <auth/auth-client.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-service.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <sqlite/db-service.hxx>

#include <feature/session/services/identity-change-consumer.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_AUTH_SCHEMA_PATH
#error "ARGUS_AUTH_SCHEMA_PATH must point at the auth schema.sql"
#endif

namespace
{
constexpr const char* kJwtSecret =
    "argus-auth-session-verdict-secret-0123456789";
constexpr const char* kFleetSecret = "argus-auth-fleet-secret-0123456789abcdef";
constexpr const char* kDeviceSecret = "00112233445566778899aabbccddeeff";
constexpr const char* kDeviceHash = "device-hash-primary";
constexpr const char* kOtherDeviceHash = "device-hash-other";
constexpr int64_t kUserId = 42;
constexpr int64_t kGatedUserId = 43;
constexpr int64_t kHourSeconds = 3600;

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
  ConfigService::setRuntimeString("jwt.refresh_secret", kJwtSecret);
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

  TempDb db_{"auth-session-verdict-test"};
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

struct SessionSeed
{
  int64_t userId{kUserId};
  std::string deviceHash{kDeviceHash};
  int64_t expiresAt{0};
  bool hashed{true};
};

[[nodiscard]] std::string issueToken(int64_t userId)
{
  static std::atomic<int> issued{0};
  JwtService jwt;
  return jwt.generateAccess({{"sub", std::to_string(userId)},
                             {"iss", "argus"},
                             {"jti", std::to_string(issued.fetch_add(1))}});
}

[[nodiscard]] std::string seedSession(const SessionSeed& seed)
{
  const std::string token = issueToken(seed.userId);
  const std::string refresh = "refresh-" + token;
  std::string storedAccess = seed.hashed ? argus::hash::sha256Hex(token) : token;
  std::string storedRefresh =
      seed.hashed ? argus::hash::sha256Hex(refresh) : refresh;
  DbService::client()->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, is_valid, is_used, expires_at) "
      "VALUES (?, ?, ?, ?, '', 1, 0, ?)",
      seed.userId, storedAccess, storedRefresh, seed.deviceHash, seed.expiresAt);
  return token;
}

void seedCredential(const std::string& secretHash)
{
  DbService::client()->execSqlSync(
      "INSERT OR IGNORE INTO device_credential (user_id, device_hash, "
      "secret_hash, is_active) VALUES (?, ?, ?, 1)",
      kUserId, kDeviceHash, secretHash);
}

class AuthRpcHarness
{
public:
  AuthRpcHarness(SessionService& sessions,
                 DeviceCredentialRepository& credentials,
                 std::string fleetSecret)
      : service_({.sessions = &sessions, .deviceCredentials = &credentials},
                 std::move(fleetSecret))
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

  [[nodiscard]] AuthClientConfig clientConfig() const
  {
    return AuthClientConfig{.target = target_, .fleetSecret = {}};
  }

  [[nodiscard]] AuthClientConfig gatedClientConfig() const
  {
    return AuthClientConfig{.target = target_, .fleetSecret = kFleetSecret};
  }

private:
  AuthRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

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

[[nodiscard]] Verdict ask(const AuthClient& client,
                          const std::string& accessToken)
{
  const auto answer = client.validateToken({.accessToken = accessToken,
                                            .deviceHash = kDeviceHash,
                                            .hasDeviceContext = true});
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

[[nodiscard]] Verdict askAccessOnly(const AuthClient& client,
                                    const std::string& accessToken)
{
  const auto answer = client.validateToken(
      {.accessToken = accessToken, .deviceHash = {}, .hasDeviceContext = false});
  if (!answer.has_value())
    return Verdict{};

  Verdict verdict;
  verdict.answered = true;
  verdict.valid = answer->valid();
  verdict.reason = answer->reason();
  verdict.expiresAt = answer->expires_at();
  return verdict;
}

[[nodiscard]] int64_t liveRowsOf(int64_t userId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM refresh_token WHERE user_id = ? "
      "AND is_valid = 1",
      userId);
  return rows.front()["total"].as<int64_t>();
}

struct IdentityChange
{
  NatsBus& bus;
  std::string subject;
  std::string msgId;
  bool isActive{true};
};

bool publishIdentityChange(const IdentityChange& change)
{
  Json::Value row(Json::objectValue);
  row["id"] = static_cast<Json::Int64>(kUserId);
  row["name"] = "Grace";
  row["isActive"] = change.isActive;

  Json::Value event(Json::objectValue);
  event[sync_change::kKindField] = sync_change::kKindIdentity;
  event[sync_change::kTableField] = tableNameToString(TableName::User);
  event[sync_change::kRecordIdField] = static_cast<Json::Int64>(kUserId);
  event[sync_change::kDeletedField] = false;
  event[sync_change::kRowField] = row;

  return change.bus.publishWithMsgId({.subject = change.subject,
                                      .payload = json_util::toString(event),
                                      .msgId = change.msgId});
}

[[nodiscard]] std::string isolatedName(const std::string& prefix)
{
  return prefix + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(tempCounter());
}
}

TEST_CASE("a valid session answers the user the filters read")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string live = seedSession({.expiresAt = now + kHourSeconds});

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  const Verdict verdict = ask(client, live);
  REQUIRE(verdict.answered);
  CHECK(verdict.valid);
  CHECK(verdict.reason.empty());
  CHECK(verdict.name == "Ada");
  CHECK(verdict.role == "owner");
  CHECK(verdict.lang == "es");
  CHECK(verdict.expiresAt == now + kHourSeconds);
}

TEST_CASE("the session verdict refuses in the identity gate's order")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string live = seedSession({.expiresAt = now + kHourSeconds});
  const std::string expired =
      seedSession({.expiresAt = now - kHourSeconds});
  const std::string otherDevice = seedSession(
      {.deviceHash = kOtherDeviceHash, .expiresAt = now + kHourSeconds});

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  const Verdict signedByNobody = ask(client, "not-a-jwt");
  REQUIRE(signedByNobody.answered);
  CHECK_FALSE(signedByNobody.valid);
  CHECK(signedByNobody.reason.empty());

  const Verdict anonymous = ask(client, issueToken(0));
  REQUIRE(anonymous.answered);
  CHECK_FALSE(anonymous.valid);
  CHECK(anonymous.reason.empty());

  app.identity().reachable = false;
  const Verdict unresolved = ask(client, live);
  REQUIRE(unresolved.answered);
  CHECK_FALSE(unresolved.valid);
  CHECK(unresolved.reason.empty());

  app.identity().reachable = true;
  app.identity().isActive = false;
  const Verdict disabled = ask(client, live);
  REQUIRE(disabled.answered);
  CHECK_FALSE(disabled.valid);
  CHECK(disabled.reason == "User account is disabled");

  app.identity().isActive = true;
  const Verdict noRow = ask(client, issueToken(kUserId));
  REQUIRE(noRow.answered);
  CHECK_FALSE(noRow.valid);
  CHECK(noRow.reason.empty());

  const Verdict expiredVerdict = ask(client, expired);
  REQUIRE(expiredVerdict.answered);
  CHECK_FALSE(expiredVerdict.valid);
  CHECK(expiredVerdict.reason == "Token expired");

  const Verdict mismatch = ask(client, otherDevice);
  REQUIRE(mismatch.answered);
  CHECK_FALSE(mismatch.valid);
  CHECK(mismatch.reason == "Device mismatch");

  const Verdict accessOnly = askAccessOnly(client, live);
  REQUIRE(accessOnly.answered);
  CHECK(accessOnly.valid);
  CHECK(accessOnly.expiresAt == 0);
}

TEST_CASE("a revoked or rotated session row refuses on both paths")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string revoked = seedSession({.expiresAt = now + kHourSeconds});
  const std::string rotated = seedSession({.expiresAt = now + kHourSeconds});
  std::string revokedHash = argus::hash::sha256Hex(revoked);
  std::string rotatedHash = argus::hash::sha256Hex(rotated);
  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET is_valid = 0 WHERE access_token = ?",
      revokedHash);
  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET is_used = 1 WHERE access_token = ?",
      rotatedHash);

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  const Verdict revokedVerdict = ask(client, revoked);
  REQUIRE(revokedVerdict.answered);
  CHECK_FALSE(revokedVerdict.valid);
  CHECK(revokedVerdict.reason.empty());

  const Verdict revokedAccessOnly = askAccessOnly(client, revoked);
  REQUIRE(revokedAccessOnly.answered);
  CHECK_FALSE(revokedAccessOnly.valid);

  const Verdict rotatedVerdict = ask(client, rotated);
  REQUIRE(rotatedVerdict.answered);
  CHECK_FALSE(rotatedVerdict.valid);

  const Verdict rotatedAccessOnly = askAccessOnly(client, rotated);
  REQUIRE(rotatedAccessOnly.answered);
  CHECK_FALSE(rotatedAccessOnly.valid);
}

TEST_CASE("tokens rest hashed, and a row written before that still verifies")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string hashed = seedSession({.expiresAt = now + kHourSeconds});
  const std::string legacy = seedSession(
      {.expiresAt = now + kHourSeconds, .hashed = false});
  CHECK(DbService::client()
            ->execSqlSync("SELECT COUNT(*) AS total FROM refresh_token "
                          "WHERE access_token = ?",
                          hashed)
            .front()["total"]
            .as<int64_t>() == 0);

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  CHECK(ask(client, hashed).valid);
  CHECK(ask(client, legacy).valid);

  const RefreshTokenSchema stored = drogon::sync_wait(
      RefreshTokenRepository{}.create({.userId = kUserId,
                                       .accessToken = "issued-access",
                                       .refreshToken = "issued-refresh",
                                       .deviceHash = kDeviceHash,
                                       .userAgent = {},
                                       .expiresAt = now + kHourSeconds,
                                       .sessionId = "issued-session",
                                       .platform = SessionPlatform::Android,
                                       .deviceName = "Pixel",
                                       .sessionCreatedAt = 0,
                                       .previousRefreshHash = "",
                                       .client = nullptr}));
  CHECK(stored.accessToken == argus::hash::sha256Hex("issued-access"));
  CHECK(drogon::sync_wait(RefreshTokenRepository{}.findByRefreshToken(
                              kUserId, "issued-refresh"))
            .has_value());
}

TEST_CASE("a revocation ends every session the user holds")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string live = seedSession({.expiresAt = now + kHourSeconds});

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  CHECK(ask(client, live).valid);
  CHECK(liveRowsOf(kUserId) > 0);

  REQUIRE(drogon::sync_wait(sessions.revokeUser(kUserId)));
  CHECK(liveRowsOf(kUserId) == 0);
  CHECK_FALSE(ask(client, live).valid);
  CHECK_FALSE(askAccessOnly(client, live).valid);
  CHECK_FALSE(drogon::sync_wait(sessions.revokeUser(kUserId)));
}

TEST_CASE("the context cache answers a repeat validation without identity")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string cached = seedSession({.expiresAt = now + kHourSeconds});

  SessionService cachedSessions(
      {.jwtService = JwtService{},
       .refreshTokenRepository = RefreshTokenRepository{},
       .identity = &app.identity()},
      SessionService::Config{.contextCacheSeconds = 30});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(cachedSessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  const int before = app.identity().calls.load();
  CHECK(ask(client, cached).valid);
  CHECK(ask(client, cached).valid);
  CHECK(app.identity().calls.load() == before + 1);

  cachedSessions.forget(kUserId);
  CHECK(ask(client, cached).valid);
  CHECK(app.identity().calls.load() == before + 2);

  SessionService uncachedSessions(
      {.jwtService = JwtService{},
       .refreshTokenRepository = RefreshTokenRepository{},
       .identity = &app.identity()},
      SessionService::Config{.contextCacheSeconds = 0});
  AuthRpcHarness plain(uncachedSessions, credentials, {});
  REQUIRE(plain.listening());
  AuthClient plainClient(plain.clientConfig());

  const int plainBefore = app.identity().calls.load();
  CHECK(ask(plainClient, cached).valid);
  CHECK(ask(plainClient, cached).valid);
  CHECK(app.identity().calls.load() == plainBefore + 2);
}

TEST_CASE("the fleet secret gates every session verdict")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string gated = seedSession(
      {.userId = kGatedUserId, .expiresAt = now + kHourSeconds});
  seedCredential(DeviceFilter::sha256Hex(kDeviceSecret));

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness gatedRpc(sessions, credentials, kFleetSecret);
  REQUIRE(gatedRpc.listening());

  AuthClient unauthorized(gatedRpc.clientConfig());
  CHECK_FALSE(unauthorized.validateToken({.accessToken = gated,
                                          .deviceHash = kDeviceHash,
                                          .hasDeviceContext = true})
                  .has_value());
  CHECK_FALSE(
      unauthorized.checkDeviceCredential(DeviceFilter::sha256Hex(kDeviceSecret))
          .has_value());

  AuthClient wrongSecret({.target = gatedRpc.clientConfig().target,
                          .fleetSecret = "not-the-fleet-secret"});
  CHECK_FALSE(wrongSecret.validateToken({.accessToken = gated,
                                         .deviceHash = kDeviceHash,
                                         .hasDeviceContext = true})
                  .has_value());

  AuthClient authorized(gatedRpc.gatedClientConfig());
  const Verdict verdict = ask(authorized, gated);
  REQUIRE(verdict.answered);
  CHECK(verdict.valid);
  CHECK(
      authorized.checkDeviceCredential(DeviceFilter::sha256Hex(kDeviceSecret)) ==
      true);
}

TEST_CASE("device credentials answer active only for a live secret hash")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  seedCredential(DeviceFilter::sha256Hex(kDeviceSecret));

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  CHECK(client.checkDeviceCredential(DeviceFilter::sha256Hex(kDeviceSecret)) ==
        true);
  CHECK(client.checkDeviceCredential(
            DeviceFilter::sha256Hex("ffeeddccbbaa99887766554433221100")) ==
        false);
  CHECK(client.checkDeviceCredential("") == false);
}

TEST_CASE("an identity change drops the cached context")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; the identity change live check skipped");
    return;
  }

  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string live = seedSession({.expiresAt = now + kHourSeconds});

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  const std::string stream = isolatedName("argus-test-auth-identity");
  const std::string subject = stream + ".change";
  const std::string durable = isolatedName("argus-test-auth");
  REQUIRE(bus.ensureStream({.name = stream,
                            .subjects = {subject},
                            .maxAgeNs = static_cast<int64_t>(kHourSeconds) *
                                        1000000000,
                            .duplicatesNs = 120LL * 1000000000}));

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 30});
  DeviceCredentialRepository credentials;
  IdentityChangeConsumer consumer(
      IdentityChangeConsumer::Dependencies{.bus = &bus, .sessions = &sessions},
      IdentityChangeConsumer::Config{.stream = stream,
                                     .durable = durable,
                                     .subject = subject,
                                     .maxDeliver = 5});
  consumer.start();

  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  CHECK(ask(client, live).name == "Ada");

  app.identity().name = "Grace";
  CHECK(ask(client, live).name == "Ada");

  REQUIRE(publishIdentityChange({.bus = bus,
                                 .subject = subject,
                                 .msgId = isolatedName("auth-identity-live")}));
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool forgotten = false;
  while (!forgotten && std::chrono::steady_clock::now() < deadline) {
    forgotten = ask(client, live).name == "Grace";
    if (!forgotten)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(forgotten);

  app.identity().isActive = false;
  REQUIRE(publishIdentityChange({.bus = bus,
                                 .subject = subject,
                                 .msgId = isolatedName("auth-identity-live"),
                                 .isActive = false}));
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool disabled = false;
  while (!disabled && std::chrono::steady_clock::now() < deadline) {
    const Verdict verdict = ask(client, live);
    disabled = !verdict.valid && verdict.reason == "User account is disabled";
    if (!disabled)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(disabled);

  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool revoked = false;
  while (!revoked && std::chrono::steady_clock::now() < deadline) {
    revoked = liveRowsOf(kUserId) == 0;
    if (!revoked)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(revoked);

  app.identity().isActive = true;
  const Verdict reEnabled = ask(client, live);
  REQUIRE(reEnabled.answered);
  CHECK_FALSE(reEnabled.valid);

  consumer.stop();
  bus.drain();
}

int main(int argc, char** argv)
{
  doctest::Context context(argc, argv);
  const int result = context.run();
  stopFixture();
  return result;
}
