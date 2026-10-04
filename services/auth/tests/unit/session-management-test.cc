#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <app/rpc/auth-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/jwt-service.hxx>
#include <auth/request-context.hxx>
#include <auth/session-platform.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/auth/dtos/revoke-sessions-dto.hxx>
#include <feature/auth/infra/client-identity.hxx>
#include <feature/auth/services/auth-feature-service.hxx>
#include <feature/auth/services/session-management-service.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/device/repositories/device-login-challenge/device-login-challenge-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <sqlite/db-service.hxx>
#include <sync/auth-change-sink.hxx>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_AUTH_SCHEMA_PATH
#error "ARGUS_AUTH_SCHEMA_PATH must point at the auth schema.sql"
#endif

namespace
{
constexpr const char* kJwtSecret =
    "argus-auth-session-management-secret-0123456789";
constexpr const char* kFingerprintSecret = "argus-session-fingerprint";
constexpr const char* kAndroidUa = "Argus/1 (android)";
constexpr const char* kDesktopUa = "Argus/1 (desktop)";
constexpr const char* kWebUa = "Argus/1 (web)";
constexpr const char* kLegacyUa = "okhttp/4.12.0";
constexpr const char* kLegacyAddress = "10.0.0.21";
constexpr int64_t kOwnerId = 1;
constexpr int64_t kOtherUserId = 2;
constexpr int64_t kLegacyUserId = 3;
constexpr int64_t kGraceSeconds = 30;

struct Refusal
{
  int status;
  std::string code;
};

template <typename T>
[[nodiscard]] std::optional<Refusal> refusalOf(drogon::Task<T> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{.status = error.statusCode(), .code = error.errorCode()};
  }
  return std::nullopt;
}

template <typename T>
[[nodiscard]] Refusal refusedBy(drogon::Task<T> task)
{
  auto refusal = refusalOf(std::move(task));
  if (!refusal)
    throw std::runtime_error("the call was not refused");
  return *refusal;
}

[[nodiscard]] std::string tempPath()
{
  return "auth-session-management-test-" + std::to_string(::getpid()) + ".db";
}

void setConfig()
{
  ConfigService::setRuntimeString("jwt.secret", kJwtSecret);
  ConfigService::setRuntimeString("jwt.refresh_secret", kJwtSecret);
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

class ScriptedIdentityClient : public IdentityClient
{
public:
  ScriptedIdentityClient() : IdentityClient("127.0.0.1:1") {}

  void setActive(int64_t userId, bool active)
  {
    const std::scoped_lock lock(mutex_);
    if (active)
      disabled_.erase(userId);
    else
      disabled_.insert(userId);
  }

  [[nodiscard]] bool active(int64_t userId) const
  {
    const std::scoped_lock lock(mutex_);
    return !disabled_.contains(userId);
  }

  [[nodiscard]] std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const override
  {
    argus::identity::v1::GetUserResponse response;
    *response.mutable_user() = identityOf(userId);
    return response;
  }

  [[nodiscard]] std::optional<argus::identity::v1::IdentifyPersonResponse>
  identifyPerson(const std::string& image) const override
  {
    const int64_t userId = std::stoll(image);
    argus::identity::v1::IdentifyPersonResponse response;
    response.set_matched(true);
    response.set_person_id(userId + 100);
    response.set_face_found(true);
    if (!active(userId)) {
      response.set_account_disabled(true);
      return response;
    }
    response.set_user_id(userId);
    response.set_role("resident");
    response.set_name("Ada");
    response.set_last_name("Rico");
    return response;
  }

  [[nodiscard]] std::optional<argus::identity::v1::RegisterUserResponse>
  registerUser(const RegisterUserInput& input) const override
  {
    const int64_t userId = std::stoll(input.image);
    argus::identity::v1::RegisterUserResponse response;
    if (!active(userId)) {
      response.set_outcome(argus::identity::v1::REGISTER_USER_ACCOUNT_DISABLED);
      return response;
    }
    response.set_outcome(argus::identity::v1::REGISTER_USER_ALREADY_REGISTERED);
    *response.mutable_user() = identityOf(userId);
    response.set_person_id(userId + 100);
    return response;
  }

private:
  [[nodiscard]] argus::identity::v1::UserIdentity identityOf(int64_t userId) const
  {
    argus::identity::v1::UserIdentity user;
    user.set_user_id(userId);
    user.set_name("Ada");
    user.set_last_name("Rico");
    user.set_lang("es");
    user.set_role(userId == kOwnerId ? "owner" : "resident");
    user.set_is_active(active(userId));
    return user;
  }

  mutable std::mutex mutex_;
  std::set<int64_t> disabled_;
};

class RecordingSink : public AuthChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    actions_.push_back(input.event.toJson());
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishSessionChange(const AuthSessionChangeInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    changes_.push_back(input.payload);
    co_return;
  }

  [[nodiscard]] std::vector<Json::Value> actions() const
  {
    const std::scoped_lock lock(mutex_);
    return actions_;
  }

  [[nodiscard]] std::vector<Json::Value> changes() const
  {
    const std::scoped_lock lock(mutex_);
    return changes_;
  }

  void clear()
  {
    const std::scoped_lock lock(mutex_);
    actions_.clear();
    changes_.clear();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<Json::Value> actions_;
  mutable std::vector<Json::Value> changes_;
};

struct LegacySeed
{
  std::string accessToken;
  std::string refreshToken;
};

[[nodiscard]] LegacySeed seedLegacySchema()
{
  auto client = DbService::client();
  client->execSqlSync(
      "CREATE TABLE refresh_token (id INTEGER NOT NULL PRIMARY KEY "
      "AUTOINCREMENT, user_id INTEGER NOT NULL, access_token TEXT NOT NULL, "
      "refresh_token TEXT NOT NULL, device_hash TEXT NOT NULL, user_agent TEXT "
      "NOT NULL DEFAULT '', is_valid INTEGER NOT NULL DEFAULT 1 CHECK "
      "(is_valid IN (0, 1)), is_used INTEGER NOT NULL DEFAULT 0 CHECK (is_used "
      "IN (0, 1)), expires_at INTEGER NOT NULL, created_at INTEGER NOT NULL "
      "DEFAULT (strftime('%s', 'now')))");
  client->execSqlSync(
      "CREATE TABLE device_login_challenge (id INTEGER NOT NULL PRIMARY KEY "
      "AUTOINCREMENT, challenge_id TEXT NOT NULL UNIQUE, device_hash TEXT NOT "
      "NULL, user_agent TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT "
      "'pending' CHECK (status IN ('pending', 'approved', 'expired')), user_id "
      "INTEGER, access_token TEXT, refresh_token TEXT, expires_at INTEGER NOT "
      "NULL, created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");

  const JwtService jwt;
  LegacySeed seed{
      .accessToken = jwt.generateAccess({{"sub", std::to_string(kLegacyUserId)}}),
      .refreshToken =
          jwt.generateRefresh({{"sub", std::to_string(kLegacyUserId)}})};
  const std::string deviceHash = DeviceFilter::addressFingerprint(
      {.userAgent = kLegacyUa, .address = kLegacyAddress});
  client->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at, created_at) VALUES (?, ?, ?, ?, ?, "
      "?, ?)",
      kLegacyUserId, argus::hash::sha256Hex(seed.accessToken),
      argus::hash::sha256Hex(seed.refreshToken), deviceHash,
      std::string(kLegacyUa), static_cast<int64_t>(std::time(nullptr)) + 3600,
      static_cast<int64_t>(std::time(nullptr)) - 7200);
  return seed;
}

class AuthRpcHarness
{
public:
  explicit AuthRpcHarness(SessionService& sessions)
      : service_({.sessions = &sessions, .deviceCredentials = &credentials_},
                 "")
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
  DeviceCredentialRepository credentials_;
  AuthRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string target_;
};

class Fixture
{
public:
  Fixture() = default;

  ~Fixture()
  {
    rpc_.reset();
    if (runner_.joinable()) {
      if (drogon::app().isRunning()) {
        drogon::app().quit();
        runner_.join();
      }
      else {
        runner_.detach();
      }
    }
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
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
                                   .filename = path_,
                                   .name = "default",
                                   .timeout = -1});
    runner_ = std::thread([] { drogon::app().run(); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!drogon::app().isRunning() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!drogon::app().isRunning())
      return false;

    legacy_ = seedLegacySchema();
    ready_ = RefreshTokenRepository().migrateLegacySchema() &&
             DeviceLoginChallengeRepository().migrateLegacySchema() &&
             DbService::runScriptFile(ARGUS_AUTH_SCHEMA_PATH);
    if (!ready_)
      return false;

    rpc_ = std::make_unique<AuthRpcHarness>(sessions_);
    ConfigService::setRuntimeString("auth.target", rpc_->target());
    auth_change::setSink(&sink_);
    return rpc_->listening();
  }

  [[nodiscard]] AuthFeatureService& auth() { return auth_; }
  [[nodiscard]] SessionManagementService& sessions() { return manager_; }
  [[nodiscard]] RecordingSink& sink() { return sink_; }
  [[nodiscard]] ScriptedIdentityClient& identity() { return identity_; }
  [[nodiscard]] SessionService& verdicts() { return sessions_; }
  [[nodiscard]] const LegacySeed& legacy() const { return legacy_; }

private:
  std::string path_{tempPath()};
  ScriptedIdentityClient identity_;
  SessionService sessions_{{.jwtService = JwtService{},
                            .refreshTokenRepository = RefreshTokenRepository{},
                            .identity = &identity_},
                           SessionService::Config{.contextCacheSeconds = 0}};
  AuthFeatureService auth_{
      {.jwtService = JwtService{},
       .refreshTokenRepository = RefreshTokenRepository{},
       .deviceCredentialRepository = DeviceCredentialRepository{},
       .challengeRepository = DeviceLoginChallengeRepository{},
       .sessions = SessionManagementService(
           {.refreshTokenRepository = RefreshTokenRepository{}}),
       .identity = &identity_},
      AuthFeatureService::Config{.refreshReuseGraceSeconds = kGraceSeconds}};
  SessionManagementService manager_{
      {.refreshTokenRepository = RefreshTokenRepository{}}};
  RecordingSink sink_;
  LegacySeed legacy_;
  std::unique_ptr<AuthRpcHarness> rpc_;
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
  if (!booted) {
    setConfig();
    booted = std::make_unique<Fixture>();
  }
  return *booted;
}

struct Device
{
  std::string userAgent;
  std::string address;
  ClientIdentity client;
};

struct OpenedSession
{
  std::string accessToken;
  std::string refreshToken;
  Device device;
};

struct OpenSessionInput
{
  int64_t userId{kOwnerId};
  Device device;
};

[[nodiscard]] std::string deviceHashOf(const Device& device)
{
  return DeviceFilter::addressFingerprint(
      {.userAgent = device.userAgent, .address = device.address});
}

[[nodiscard]] OpenedSession openSession(const OpenSessionInput& input)
{
  AuthFeatureService& auth = fixture().auth();
  const LoginDeviceInput login{.deviceHash = deviceHashOf(input.device),
                               .userAgent = input.device.userAgent,
                               .client = input.device.client};
  const auto challenge = drogon::sync_wait(
      auth.createDeviceLogin({.device = login, .pollHash = ""}));
  drogon::sync_wait(auth.approveDeviceLogin(challenge.challengeId, input.userId));
  const auto polled = drogon::sync_wait(auth.pollDeviceLogin(
      {.challengeId = challenge.challengeId, .device = login, .proof = ""}));
  return OpenedSession{.accessToken = polled.accessToken,
                       .refreshToken = polled.refreshToken,
                       .device = input.device};
}

[[nodiscard]] drogon::HttpRequestPtr requestFrom(const OpenedSession& session)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->addHeader("User-Agent", session.device.userAgent);
  req->addHeader("X-Forwarded-For", session.device.address);
  req->addHeader("Authorization", "Bearer " + session.accessToken);
  return req;
}

[[nodiscard]] std::optional<JwtContext> authenticate(const OpenedSession& session)
{
  const auto req = requestFrom(session);
  DeviceFilter deviceFilter;
  JwtFilter jwtFilter;
  try {
    drogon::sync_wait(deviceFilter.doFilter(req));
    drogon::sync_wait(jwtFilter.doFilter(req));
  }
  catch (const ResponseException&) {
    return std::nullopt;
  }
  return req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
}

[[nodiscard]] JwtContext contextOf(const OpenedSession& session)
{
  auto context = authenticate(session);
  if (!context)
    throw std::runtime_error("the session did not authenticate");
  return *context;
}

struct RefreshAttempt
{
  std::string refreshToken;
  Device device;
};

[[nodiscard]] RefreshTokenInput refreshInputOf(const RefreshAttempt& attempt)
{
  return RefreshTokenInput{.body = {.refreshToken = attempt.refreshToken},
                           .deviceHash = deviceHashOf(attempt.device),
                           .userAgent = attempt.device.userAgent,
                           .ip = attempt.device.address,
                           .credentialHash = "",
                           .client = attempt.device.client};
}

[[nodiscard]] std::vector<std::string> activeSessionIds(int64_t userId)
{
  const auto rows = drogon::sync_wait(RefreshTokenRepository().listActive(
      {.userId = userId,
       .now = static_cast<int64_t>(std::time(nullptr)),
       .client = nullptr}));
  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows)
    ids.push_back(row.sessionId);
  return ids;
}

[[nodiscard]] bool contains(const std::vector<std::string>& ids,
                            const std::string& id)
{
  return std::ranges::find(ids, id) != ids.end();
}

[[nodiscard]] bool mentionsSecret(const Json::Value& payload)
{
  const std::string text = json_util::toString(payload);
  return std::ranges::any_of(
      std::vector<std::string>{"eyJ", "\"accessToken\"", "\"refreshToken\"",
                               "\"deviceHash\"", "\"userAgent\""},
      [&text](const std::string& marker) {
        return text.find(marker) != std::string::npos;
      });
}

[[nodiscard]] Device androidPhone()
{
  return Device{.userAgent = kAndroidUa,
                .address = "10.0.0.11",
                .client = {.platform = SessionPlatform::Android,
                           .deviceName = "Pixel 8"}};
}

[[nodiscard]] Device desktop()
{
  return Device{.userAgent = kDesktopUa,
                .address = "10.0.0.12",
                .client = {.platform = SessionPlatform::Desktop,
                           .deviceName = ""}};
}

[[nodiscard]] Device browser()
{
  return Device{.userAgent = kWebUa,
                .address = "10.0.0.13",
                .client = {.platform = SessionPlatform::Web,
                           .deviceName = "Firefox"}};
}
}

TEST_CASE("a session written before the migration keeps working with an id")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const OpenedSession legacy{
      .accessToken = app.legacy().accessToken,
      .refreshToken = app.legacy().refreshToken,
      .device = {.userAgent = kLegacyUa, .address = kLegacyAddress, .client = {}}};
  const JwtContext context = contextOf(legacy);
  CHECK(context.sub == kLegacyUserId);
  CHECK(SessionManagementService::isSessionId(context.sessionId));

  const auto listed = drogon::sync_wait(app.sessions().list(
      {.userId = kLegacyUserId, .currentSessionId = context.sessionId}));
  REQUIRE(listed.sessions.size() == 1);
  CHECK(listed.sessions.front().current);
  CHECK(listed.sessions.front().platform == SessionPlatform::Unknown);
  CHECK(listed.sessions.front().createdAt > 0);

  const auto columns = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM pragma_table_info('device_login_challenge') "
      "WHERE name IN ('platform', 'device_name')");
  CHECK(columns.front()["total"].as<int64_t>() == 2);
}

TEST_CASE("the client identity headers name the platform and the device")
{
  CHECK(client_identity::platformOfStableUserAgent("Argus/1 (ios)") ==
        SessionPlatform::Ios);
  CHECK_FALSE(client_identity::platformOfStableUserAgent("Argus/1 (tv)"));
  CHECK_FALSE(client_identity::isStableUserAgent("okhttp/4.12.0"));
  CHECK_FALSE(client_identity::isStableUserAgent(""));
  CHECK(client_identity::platformOfClientHeader("android/1.4.0") ==
        SessionPlatform::Android);
  CHECK(client_identity::platformOfClientHeader("toaster/1.0") ==
        SessionPlatform::Unknown);
  CHECK(client_identity::platformOfClientHeader("web") ==
        SessionPlatform::Unknown);

  CHECK(client_identity::decodeDeviceName("Pixel%208") == "Pixel 8");
  CHECK(client_identity::decodeDeviceName("Port%C3%A1til%20de%20Ana") ==
        "Portátil de Ana");
  CHECK(client_identity::decodeDeviceName("a%0Ab%09c%7F") == "abc");
  CHECK(client_identity::decodeDeviceName("%E2%28%A1").empty());
  CHECK(client_identity::decodeDeviceName("%4").empty());
  CHECK(client_identity::decodeDeviceName("%20%20").empty());
  const std::string longName(100, 'x');
  CHECK(client_identity::decodeDeviceName(longName).size() ==
        client_identity::kMaxDeviceNameChars);
  std::string accents;
  for (int i = 0; i < 70; ++i)
    accents += "%C3%B1";
  CHECK(client_identity::decodeDeviceName(accents).size() ==
        client_identity::kMaxDeviceNameChars * 2);

  auto req = drogon::HttpRequest::newHttpRequest();
  req->addHeader("User-Agent", "Argus/1 (desktop)");
  req->addHeader("X-Argus-Device", "Mesa%20de%20David");
  const ClientIdentity fromAgent = client_identity::of(req);
  CHECK(fromAgent.platform == SessionPlatform::Desktop);
  CHECK(fromAgent.deviceName == "Mesa de David");
  req->addHeader("X-Argus-Client", "web/1.4.0");
  CHECK(client_identity::of(req).platform == SessionPlatform::Web);
}

TEST_CASE("an owner lists, revokes one session and the rest, and logs out")
{
  Fixture& app = fixture();
  REQUIRE(app.start());
  app.sink().clear();

  const auto phone = openSession({.userId = kOwnerId, .device = androidPhone()});
  const auto desk = openSession({.userId = kOwnerId, .device = desktop()});
  const auto web = openSession({.userId = kOwnerId, .device = browser()});
  const auto stranger =
      openSession({.userId = kOtherUserId, .device = androidPhone()});
  CHECK(app.sink().changes().size() == 8);

  const JwtContext phoneContext = contextOf(phone);
  const JwtContext deskContext = contextOf(desk);
  const JwtContext webContext = contextOf(web);
  const JwtContext strangerContext = contextOf(stranger);
  const std::string& phoneId = phoneContext.sessionId;
  CHECK(SessionManagementService::isSessionId(phoneId));
  CHECK(phoneId != deskContext.sessionId);

  const auto listed = drogon::sync_wait(
      app.sessions().list({.userId = kOwnerId, .currentSessionId = phoneId}));
  REQUIRE(listed.sessions.size() == 3);
  CHECK(listed.sessions.front().id == phoneId);
  CHECK(listed.sessions.front().current);
  CHECK(listed.sessions.front().platform == SessionPlatform::Android);
  CHECK(listed.sessions.front().deviceName == "Pixel 8");
  const Json::Value json = listed.toJson();
  for (const auto& item : json["sessions"]) {
    CHECK(item.getMemberNames() ==
          std::vector<std::string>{"createdAt", "current", "deviceName",
                                   "expiresAt", "id", "lastSeenAt",
                                   "platform"});
  }
  const auto deskItem = std::ranges::find_if(json["sessions"], [&](const Json::Value& item) {
    return item["id"].asString() == deskContext.sessionId;
  });
  REQUIRE(deskItem != json["sessions"].end());
  CHECK((*deskItem)["platform"].asString() == "desktop");
  CHECK((*deskItem)["deviceName"].isNull());
  CHECK_FALSE((*deskItem)["current"].asBool());

  app.sink().clear();
  const auto revoked = drogon::sync_wait(app.sessions().revokeOne(
      {.owner = {.userId = kOwnerId, .currentSessionId = phoneId},
       .sessionId = deskContext.sessionId}));
  CHECK(revoked.revoked == std::vector<std::string>{deskContext.sessionId});
  CHECK_FALSE(revoked.current);

  const auto refusedAt = std::chrono::steady_clock::now();
  CHECK_FALSE(authenticate(desk).has_value());
  CHECK(std::chrono::steady_clock::now() - refusedAt < std::chrono::seconds(1));
  CHECK(authenticate(phone).has_value());

  const auto changes = app.sink().changes();
  REQUIRE(changes.size() == 3);
  CHECK(changes[0]["action"].asString() == "disconnect_session");
  CHECK(changes[0]["user"].asInt64() == kOwnerId);
  CHECK(changes[0]["session"].asString() == deskContext.sessionId);
  CHECK(changes[0]["operation"].asInt() == 7);
  CHECK(changes[0]["info"]["reason"].asString() == "sessionRevoked");
  CHECK(changes[0]["info"]["sessionId"].asString() == deskContext.sessionId);
  CHECK_FALSE(changes[0]["info"]["resync"].asBool());
  CHECK(changes[1]["info"]["reason"].asString() == "sessionsChanged");
  CHECK(changes[1]["users"][0].asInt64() == kOwnerId);
  CHECK_FALSE(changes[1].isMember("action"));
  CHECK(changes[0]["info"]["cause"].asString() == "revoked");
  CHECK(changes[2]["option"].asString() == "user_invitation");
  CHECK(changes[2]["info"]["reason"].asString() == "userSessionsChanged");
  CHECK(changes[2]["info"]["userId"].asInt64() == kOwnerId);
  CHECK_FALSE(changes[2].isMember("users"));
  CHECK_FALSE(changes[2].isMember("action"));
  const auto actions = app.sink().actions();
  REQUIRE(actions.size() == 1);
  CHECK(actions[0]["old_data"]["sessionId"].asString() ==
        deskContext.sessionId);
  CHECK(actions[0]["old_data"]["platform"].asString() == "desktop");
  CHECK(actions[0]["new_data"]["scope"].asString() == "one");
  CHECK(actions[0]["new_data"]["reason"].asString() == "revoked");
  CHECK(actions[0]["new_data"]["revokedBy"].asInt64() == kOwnerId);
  for (const auto& payload : changes)
    CHECK_FALSE(mentionsSecret(payload));
  CHECK_FALSE(mentionsSecret(actions[0]));

  const Refusal again = refusedBy(app.sessions().revokeOne(
      {.owner = {.userId = kOwnerId, .currentSessionId = phoneId},
       .sessionId = deskContext.sessionId}));
  CHECK(again.status == 404);
  CHECK(again.code == "SESSION_NOT_FOUND");
  const Refusal foreign = refusedBy(app.sessions().revokeOne(
      {.owner = {.userId = kOwnerId, .currentSessionId = phoneId},
       .sessionId = strangerContext.sessionId}));
  CHECK(foreign.status == 404);
  CHECK(authenticate(stranger).has_value());
  const Refusal malformed = refusedBy(app.sessions().revokeOne(
      {.owner = {.userId = kOwnerId, .currentSessionId = phoneId},
       .sessionId = "../../etc"}));
  CHECK(malformed.code == "SESSION_NOT_FOUND");

  const auto second = openSession({.userId = kOwnerId, .device = desktop()});
  app.sink().clear();
  const auto others = drogon::sync_wait(app.sessions().revokeScope(
      {.owner = {.userId = kOwnerId, .currentSessionId = phoneId},
       .scope = SessionRevocationScope::Others}));
  CHECK(others.revoked.size() == 2);
  CHECK_FALSE(others.current);
  CHECK(contains(others.revoked, webContext.sessionId));
  CHECK(activeSessionIds(kOwnerId) == std::vector<std::string>{phoneId});
  CHECK_FALSE(authenticate(web).has_value());
  CHECK_FALSE(authenticate(second).has_value());
  CHECK(authenticate(phone).has_value());
  CHECK(app.sink().changes().size() == 4);

  const auto spare = openSession({.userId = kOwnerId, .device = browser()});
  REQUIRE(authenticate(spare).has_value());
  drogon::sync_wait(
      app.auth().logout({.userId = kOwnerId, .sessionId = phoneId}));
  CHECK_FALSE(authenticate(phone).has_value());
  CHECK(authenticate(spare).has_value());
  CHECK(authenticate(stranger).has_value());

  const JwtContext spareContext = contextOf(spare);
  const auto all = drogon::sync_wait(app.sessions().revokeScope(
      {.owner = {.userId = kOwnerId,
                 .currentSessionId = spareContext.sessionId},
       .scope = SessionRevocationScope::All}));
  CHECK(all.current);
  CHECK(all.revoked == std::vector<std::string>{spareContext.sessionId});
  CHECK(activeSessionIds(kOwnerId).empty());
  CHECK(authenticate(stranger).has_value());
}

TEST_CASE("the scope of a bulk revocation is validated")
{
  const auto scopeOf = [](const std::string& query) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setParameter("scope", query);
    return RevokeSessionsDto::fromRequest(req).target;
  };
  CHECK(scopeOf("others") == SessionRevocationScope::Others);
  CHECK(scopeOf("all") == SessionRevocationScope::All);
  CHECK_THROWS(scopeOf("everything"));
  CHECK_THROWS(scopeOf(""));
}

TEST_CASE("a rotated refresh token races inside the window and is a theft outside it")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const auto phone = openSession({.userId = kOtherUserId, .device = androidPhone()});
  const JwtContext context = contextOf(phone);
  const std::string sessionId = context.sessionId;

  const auto first = drogon::sync_wait(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = phone.refreshToken, .device = phone.device})));
  CHECK(JwtService().verifyRefresh(first.refreshToken).at("sid") == sessionId);

  app.sink().clear();
  const Refusal raced = refusedBy(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = phone.refreshToken, .device = phone.device})));
  CHECK(raced.status == 401);
  CHECK(contains(activeSessionIds(kOtherUserId), sessionId));
  CHECK(app.sink().changes().empty());

  const auto second = drogon::sync_wait(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = first.refreshToken, .device = phone.device})));
  CHECK_FALSE(second.refreshToken.empty());

  const Refusal replayed = refusedBy(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = phone.refreshToken, .device = phone.device})));
  CHECK(replayed.status == 401);
  CHECK_FALSE(contains(activeSessionIds(kOtherUserId), sessionId));
  const auto changes = app.sink().changes();
  REQUIRE(changes.size() == 3);
  CHECK(changes[0]["session"].asString() == sessionId);
  const auto actions = app.sink().actions();
  REQUIRE(actions.size() == 1);
  CHECK(actions[0]["new_data"]["reason"].asString() == "refreshTokenReuse");
  CHECK_FALSE(mentionsSecret(actions[0]));
  CHECK(refusalOf(app.auth().refreshToken(refreshInputOf(
                      {.refreshToken = second.refreshToken,
                       .device = phone.device})))
            .has_value());

  const auto late = openSession({.userId = kOtherUserId, .device = desktop()});
  const JwtContext lateContext = contextOf(late);
  const auto rotated = drogon::sync_wait(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = late.refreshToken, .device = late.device})));
  CHECK_FALSE(rotated.refreshToken.empty());
  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET created_at = created_at - ? "
      "WHERE session_id = ?",
      kGraceSeconds + 5, lateContext.sessionId);
  CHECK(refusalOf(app.auth().refreshToken(refreshInputOf(
                      {.refreshToken = late.refreshToken,
                       .device = late.device})))
            .has_value());
  CHECK_FALSE(contains(activeSessionIds(kOtherUserId), lateContext.sessionId));

  const auto stolen = openSession({.userId = kOtherUserId, .device = browser()});
  const JwtContext stolenContext = contextOf(stolen);
  CHECK_FALSE(drogon::sync_wait(app.auth().refreshToken(refreshInputOf(
                                    {.refreshToken = stolen.refreshToken,
                                     .device = stolen.device})))
                  .refreshToken.empty());
  Device thief = stolen.device;
  thief.userAgent = "curl/8.0";
  CHECK(refusalOf(app.auth().refreshToken(refreshInputOf(
                      {.refreshToken = stolen.refreshToken, .device = thief})))
            .has_value());
  CHECK_FALSE(
      contains(activeSessionIds(kOtherUserId), stolenContext.sessionId));
}

TEST_CASE("a legacy agent moves to the stable agent once, from the device it is bound to")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const auto rows = DbService::client()->execSqlSync(
      "SELECT session_id FROM refresh_token WHERE user_id = ? AND is_valid = 1 "
      "AND is_used = 0",
      kLegacyUserId);
  REQUIRE(rows.size() == 1);
  const std::string sessionId = rows.front()["session_id"].as<std::string>();

  const Device elsewhere{.userAgent = kAndroidUa,
                         .address = "10.0.0.99",
                         .client = {.platform = SessionPlatform::Android,
                                    .deviceName = "Pixel"}};
  CHECK(refusalOf(app.auth().refreshToken(refreshInputOf(
                      {.refreshToken = app.legacy().refreshToken,
                       .device = elsewhere})))
            .has_value());

  Device upgraded = elsewhere;
  upgraded.address = kLegacyAddress;
  const auto moved = drogon::sync_wait(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = app.legacy().refreshToken, .device = upgraded})));
  CHECK(JwtService().verifyRefresh(moved.refreshToken).at("sid") == sessionId);
  const OpenedSession movedSession{.accessToken = moved.accessToken,
                                   .refreshToken = moved.refreshToken,
                                   .device = upgraded};
  const JwtContext context = contextOf(movedSession);
  CHECK(context.sessionId == sessionId);
  const auto listed = drogon::sync_wait(app.sessions().list(
      {.userId = kLegacyUserId, .currentSessionId = sessionId}));
  REQUIRE(listed.sessions.size() == 1);
  CHECK(listed.sessions.front().platform == SessionPlatform::Android);
  CHECK(listed.sessions.front().deviceName == "Pixel");

  Device switched = upgraded;
  switched.userAgent = "Argus/1 (ios)";
  CHECK(refusalOf(app.auth().refreshToken(refreshInputOf(
                      {.refreshToken = moved.refreshToken, .device = switched})))
            .has_value());
  CHECK(contains(activeSessionIds(kLegacyUserId), sessionId));

  ConfigService::setRuntimeString("device.identity_mode", "credential");
  const std::string secretHash = DeviceFilter::sha256Hex("device-secret-a");
  const JwtService jwt;
  const std::string legacyRefresh = jwt.generateRefresh({{"sub", "3"}});
  DbService::client()->execSqlSync(
      "INSERT INTO refresh_token (user_id, access_token, refresh_token, "
      "device_hash, user_agent, expires_at) VALUES (?, ?, ?, ?, ?, ?)",
      kLegacyUserId,
      argus::hash::sha256Hex(jwt.generateAccess({{"sub", "3"}})),
      argus::hash::sha256Hex(legacyRefresh),
      DeviceFilter::credentialFingerprint(kLegacyUa, secretHash),
      std::string(kLegacyUa), static_cast<int64_t>(std::time(nullptr)) + 3600);
  const auto credentialRefresh = [&](const std::string& presentedHash) {
    return RefreshTokenInput{
        .body = {.refreshToken = legacyRefresh},
        .deviceHash = DeviceFilter::credentialFingerprint(kAndroidUa, presentedHash),
        .userAgent = kAndroidUa,
        .ip = "10.9.9.9",
        .credentialHash = presentedHash,
        .client = {.platform = SessionPlatform::Android, .deviceName = ""}};
  };
  CHECK(refusalOf(app.auth().refreshToken(
                      credentialRefresh(DeviceFilter::sha256Hex("device-secret-b"))))
            .has_value());
  CHECK_FALSE(drogon::sync_wait(app.auth().refreshToken(
                                    credentialRefresh(secretHash)))
                  .refreshToken.empty());
  ConfigService::setRuntimeString("device.identity_mode", "ip");
}

TEST_CASE("last seen advances on use at most once a minute")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const auto phone = openSession({.userId = kOwnerId, .device = androidPhone()});
  const JwtContext context = contextOf(phone);
  const auto lastSeen = [&] {
    return DbService::client()
        ->execSqlSync("SELECT last_seen_at FROM refresh_token WHERE "
                      "session_id = ? AND is_used = 0",
                      context.sessionId)
        .front()["last_seen_at"]
        .as<int64_t>();
  };

  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET last_seen_at = last_seen_at - 3600 "
      "WHERE session_id = ?",
      context.sessionId);
  const int64_t stale = lastSeen();
  REQUIRE(authenticate(phone).has_value());
  const int64_t touched = lastSeen();
  CHECK(touched >= stale + 3600);

  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET last_seen_at = last_seen_at - 30 "
      "WHERE session_id = ?",
      context.sessionId);
  REQUIRE(authenticate(phone).has_value());
  CHECK(lastSeen() == touched - 30);
}

TEST_CASE("the owner lists every user's sessions and closes another user's one or all")
{
  Fixture& app = fixture();
  REQUIRE(app.start());
  constexpr int64_t kTargetId = 21;
  constexpr int64_t kBystanderId = 22;

  const auto owner = openSession({.userId = kOwnerId, .device = desktop()});
  const auto phone = openSession({.userId = kTargetId, .device = androidPhone()});
  const auto laptop = openSession({.userId = kTargetId, .device = browser()});
  const auto bystander =
      openSession({.userId = kBystanderId, .device = androidPhone()});
  const JwtContext ownerContext = contextOf(owner);
  const JwtContext phoneContext = contextOf(phone);
  const JwtContext laptopContext = contextOf(laptop);
  const JwtContext bystanderContext = contextOf(bystander);
  const SessionOwnerInput actor{.userId = kOwnerId,
                                .currentSessionId = ownerContext.sessionId};

  const auto overview = drogon::sync_wait(app.sessions().listEveryUser(actor));
  const auto userOf = [&overview](int64_t userId) {
    return std::ranges::find(overview.users, userId, &UserSessionsView::userId);
  };
  REQUIRE(userOf(kTargetId) != overview.users.end());
  REQUIRE(userOf(kOwnerId) != overview.users.end());
  CHECK(userOf(kTargetId)->sessions.size() == 2);
  CHECK(std::ranges::none_of(userOf(kTargetId)->sessions, &SessionView::current));
  CHECK(std::ranges::any_of(userOf(kOwnerId)->sessions, &SessionView::current));
  const Json::Value overviewJson = overview.toJson();
  for (const auto& user : overviewJson["users"]) {
    CHECK(user.getMemberNames() == std::vector<std::string>{"sessions", "userId"});
    CHECK_FALSE(mentionsSecret(user));
  }

  const auto listed = drogon::sync_wait(
      app.sessions().listOfUser({.actor = actor, .userId = kTargetId}));
  CHECK(listed.sessions.size() == 2);
  CHECK(std::ranges::none_of(listed.sessions, &SessionView::current));

  app.sink().clear();
  const auto one = drogon::sync_wait(app.sessions().revokeUserSession(
      {.actor = actor, .userId = kTargetId, .sessionId = phoneContext.sessionId}));
  CHECK(one.revoked == std::vector<std::string>{phoneContext.sessionId});
  CHECK_FALSE(one.current);
  CHECK_FALSE(authenticate(phone).has_value());
  CHECK(authenticate(laptop).has_value());
  CHECK(authenticate(owner).has_value());
  const auto changes = app.sink().changes();
  REQUIRE(changes.size() == 3);
  CHECK(changes[0]["action"].asString() == "disconnect_session");
  CHECK(changes[0]["user"].asInt64() == kTargetId);
  CHECK(changes[0]["info"]["cause"].asString() == "revokedByOwner");
  CHECK(changes[1]["users"][0].asInt64() == kTargetId);
  CHECK(changes[2]["info"]["userId"].asInt64() == kTargetId);
  const auto actions = app.sink().actions();
  REQUIRE(actions.size() == 1);
  CHECK(actions[0]["record_id"].asInt64() == kTargetId);
  CHECK(actions[0]["new_data"]["reason"].asString() == "revokedByOwner");
  CHECK(actions[0]["new_data"]["revokedBy"].asInt64() == kOwnerId);
  CHECK_FALSE(mentionsSecret(actions[0]));

  const Refusal again = refusedBy(app.sessions().revokeUserSession(
      {.actor = actor, .userId = kTargetId, .sessionId = phoneContext.sessionId}));
  CHECK(again.status == 404);
  const Refusal crossed = refusedBy(app.sessions().revokeUserSession(
      {.actor = actor,
       .userId = kTargetId,
       .sessionId = bystanderContext.sessionId}));
  CHECK(crossed.code == "SESSION_NOT_FOUND");
  CHECK(authenticate(bystander).has_value());

  const auto all = drogon::sync_wait(app.sessions().revokeUserSessions(
      {.actor = actor, .userId = kTargetId}));
  CHECK(all.revoked == std::vector<std::string>{laptopContext.sessionId});
  CHECK_FALSE(all.current);
  CHECK(activeSessionIds(kTargetId).empty());
  CHECK(authenticate(bystander).has_value());
  CHECK(authenticate(owner).has_value());

  const auto selfAll = drogon::sync_wait(app.sessions().revokeUserSessions(
      {.actor = actor, .userId = kOwnerId}));
  CHECK(selfAll.current);
  CHECK(contains(selfAll.revoked, ownerContext.sessionId));
  CHECK_FALSE(authenticate(owner).has_value());
}

TEST_CASE("a disabled account loses every session and is refused at every way in")
{
  Fixture& app = fixture();
  REQUIRE(app.start());
  constexpr int64_t kDisabledId = 31;
  constexpr int64_t kLaggingId = 32;
  constexpr int64_t kQrId = 33;
  const auto phoneLogin = [] {
    const Device device = androidPhone();
    return LoginDeviceInput{.deviceHash = deviceHashOf(device),
                            .userAgent = device.userAgent,
                            .client = device.client};
  };

  const auto phone = openSession({.userId = kDisabledId, .device = androidPhone()});
  const auto web = openSession({.userId = kDisabledId, .device = browser()});
  app.sink().clear();
  app.identity().setActive(kDisabledId, false);
  CHECK(drogon::sync_wait(app.verdicts().revokeUser(kDisabledId)));
  CHECK_FALSE(authenticate(phone).has_value());
  CHECK_FALSE(authenticate(web).has_value());
  CHECK(activeSessionIds(kDisabledId).empty());
  const auto changes = app.sink().changes();
  const auto revokedFrames = std::ranges::count_if(changes, [](const Json::Value& change) {
    return change["action"].asString() == "disconnect_session" &&
           change["info"]["cause"].asString() == "accountDisabled";
  });
  CHECK(revokedFrames == 2);
  for (const auto& action : app.sink().actions())
    CHECK(action["new_data"]["reason"].asString() == "accountDisabled");

  const Refusal face =
      refusedBy(app.auth().login(LoginDto{.image = std::to_string(kDisabledId)},
                                 phoneLogin()));
  CHECK(face.status == 403);
  CHECK(face.code == "ACCOUNT_DISABLED");
  const Refusal registered = refusedBy(app.auth().registerUser(
      RegisterDto{.image = std::to_string(kDisabledId),
                  .name = "Ada",
                  .inviteCode = "",
                  .lang = "es"},
      phoneLogin()));
  CHECK(registered.status == 403);
  CHECK(registered.code == "ACCOUNT_DISABLED");
  const Refusal oldRefresh = refusedBy(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = phone.refreshToken, .device = phone.device})));
  CHECK(oldRefresh.status == 403);
  CHECK(oldRefresh.code == "ACCOUNT_DISABLED");

  const auto lagging = openSession({.userId = kLaggingId, .device = androidPhone()});
  app.identity().setActive(kLaggingId, false);
  const Refusal refresh = refusedBy(app.auth().refreshToken(refreshInputOf(
      {.refreshToken = lagging.refreshToken, .device = lagging.device})));
  CHECK(refresh.status == 403);
  CHECK(refresh.code == "ACCOUNT_DISABLED");
  CHECK(activeSessionIds(kLaggingId).empty());

  const LoginDeviceInput qrDevice = phoneLogin();
  const auto challenge = drogon::sync_wait(
      app.auth().createDeviceLogin({.device = qrDevice, .pollHash = ""}));
  drogon::sync_wait(app.auth().approveDeviceLogin(challenge.challengeId, kQrId));
  app.identity().setActive(kQrId, false);
  const auto polled = drogon::sync_wait(app.auth().pollDeviceLogin(
      {.challengeId = challenge.challengeId, .device = qrDevice, .proof = ""}));
  CHECK(polled.status == DeviceLoginStatus::Expired);
  CHECK(polled.accessToken.empty());
  CHECK(polled.refreshToken.empty());
  const Refusal approve = refusedBy(app.auth().approveDeviceLogin(
      drogon::sync_wait(
          app.auth().createDeviceLogin({.device = qrDevice, .pollHash = ""}))
          .challengeId,
      kQrId));
  CHECK(approve.status == 403);

  app.identity().setActive(kDisabledId, true);
  const auto back = drogon::sync_wait(app.auth().login(
      LoginDto{.image = std::to_string(kDisabledId)}, phoneLogin()));
  CHECK(back.userId == kDisabledId);
  CHECK(activeSessionIds(kDisabledId).size() == 1);
  CHECK_FALSE(authenticate(phone).has_value());
  CHECK_FALSE(authenticate(web).has_value());
}

int main(int argc, char** argv)
{
  doctest::Context context(argc, argv);
  const int result = context.run();
  auth_change::setSink(nullptr);
  fixtureStorage().reset();
  return result;
}
