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
#include <feature/session/services/presence-signal.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <session-fixture.hxx>
#include <verdict-golden.hxx>
#include <sqlite/db-service.hxx>

#include <feature/session/services/identity-change-consumer.hxx>
#include <json/value.h>
#include <nats/live-broker.hxx>
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
#include <vector>
#include <unistd.h>

namespace
{

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

[[nodiscard]] Verdict askAccessOnly(const AuthClient& client,
                                    const std::string& accessToken)
{
  const auto answer = client.validateToken(
      {.accessToken = accessToken,
       .deviceHash = {},
       .hasDeviceContext = false,
       .origin = {}});
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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

class RecordingPresenceSink final : public PresenceSignalSink
{
public:
  void publish(const PresenceSignal& signal) override
  {
    signals.push_back(signal);
  }

  std::vector<PresenceSignal> signals;
};

[[nodiscard]] bool askFrom(const AuthClient& client,
                           const std::string& accessToken,
                           const std::string& origin)
{
  const auto answer = client.validateToken({.accessToken = accessToken,
                                            .deviceHash = kDeviceHash,
                                            .hasDeviceContext = true,
                                            .origin = origin});
  return answer.has_value() && answer->valid();
}

TEST_CASE("a verdict tells presence where a session comes from, at most once "
          "a minute per origin")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  const int64_t now = std::time(nullptr);
  const std::string live = seedSession({.expiresAt = now + kHourSeconds});

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  RecordingPresenceSink sink;
  sessions.setPresenceSink(&sink);
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());
  AuthClient client(harness.clientConfig());

  REQUIRE(askFrom(client, live, "lan"));
  REQUIRE(sink.signals.size() == 1);
  CHECK(sink.signals.front().userId == kUserId);
  CHECK(sink.signals.front().origin == SessionOrigin::Lan);
  CHECK(sink.signals.front().sessionId.size() == 32);
  CHECK(sink.signals.front().at >= now);

  REQUIRE(askFrom(client, live, "lan"));
  CHECK(sink.signals.size() == 1);

  REQUIRE(askFrom(client, live, "tunnel"));
  REQUIRE(sink.signals.size() == 2);
  CHECK(sink.signals.back().origin == SessionOrigin::Tunnel);

  REQUIRE(askFrom(client, live, "loopback"));
  REQUIRE(askFrom(client, live, "external"));
  REQUIRE(askFrom(client, live, ""));
  CHECK(sink.signals.size() == 2);

  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET last_seen_at = last_seen_at - 120");
  REQUIRE(askFrom(client, live, "tunnel"));
  CHECK(sink.signals.size() == 3);

  CHECK_FALSE(askFrom(client, issueToken(kUserId), "lan"));
  CHECK(sink.signals.size() == 3);
  sessions.setPresenceSink(nullptr);
}

TEST_CASE("the presence throttle forgets nothing it needs and keeps no "
          "unbounded map")
{
  PresenceSignalThrottle throttle;
  CHECK_FALSE(throttle.admit({.sessionId = "",
                              .origin = SessionOrigin::Lan,
                              .seenAdvanced = true}));
  CHECK_FALSE(throttle.admit({.sessionId = "s",
                              .origin = SessionOrigin::Unknown,
                              .seenAdvanced = true}));
  CHECK(throttle.admit(
      {.sessionId = "s", .origin = SessionOrigin::Lan, .seenAdvanced = false}));
  CHECK_FALSE(throttle.admit(
      {.sessionId = "s", .origin = SessionOrigin::Lan, .seenAdvanced = false}));
  CHECK(throttle.admit(
      {.sessionId = "s", .origin = SessionOrigin::Lan, .seenAdvanced = true}));
  for (std::size_t index = 0; index <= PresenceSignalThrottle::kMaxSessions;
       ++index)
    CHECK(throttle.admit({.sessionId = "x" + std::to_string(index),
                          .origin = SessionOrigin::Tunnel,
                          .seenAdvanced = false}));
  CHECK(throttle.admit(
      {.sessionId = "s", .origin = SessionOrigin::Lan, .seenAdvanced = false}));
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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
                                       .networkHash = "",
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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
      {.jwtService = JwtService{JwtRole::Issuer},
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
      {.jwtService = JwtService{JwtRole::Issuer},
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness gatedRpc(sessions, credentials, kFleetSecret);
  REQUIRE(gatedRpc.listening());

  AuthClient unauthorized(gatedRpc.clientConfig());
  CHECK_FALSE(unauthorized.validateToken({.accessToken = gated,
                                          .deviceHash = kDeviceHash,
                                          .hasDeviceContext = true,
                                          .origin = {}})
                  .has_value());
  CHECK_FALSE(
      unauthorized.checkDeviceCredential(DeviceFilter::sha256Hex(kDeviceSecret))
          .has_value());

  AuthClient wrongSecret({.target = gatedRpc.clientConfig().target,
                          .fleetSecret = "not-the-fleet-secret"});
  CHECK_FALSE(wrongSecret.validateToken({.accessToken = gated,
                                         .deviceHash = kDeviceHash,
                                         .hasDeviceContext = true,
                                         .origin = {}})
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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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

TEST_CASE("an identity change drops the cached context" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();

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

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
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

TEST_CASE("the inline verdict path answers the recorded golden")
{
  Fixture& app = fixture();
  REQUIRE(app.start());

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = &app.identity()},
                          SessionService::Config{.contextCacheSeconds = 0});
  DeviceCredentialRepository credentials;
  AuthRpcHarness harness(sessions, credentials, {});
  REQUIRE(harness.listening());

  const auto verdicts =
      askGoldenVerdicts(harness.clientConfig().target, app.identity());
  REQUIRE(verdicts.size() == 6);
  if (recordingGolden()) {
    recordGolden(verdicts);
    MESSAGE("recorded the golden ValidateToken answers");
    return;
  }
  const Json::Value golden = goldenFile();
  CHECK(golden.size() == verdicts.size());
  for (const auto& verdict : verdicts) {
    INFO(verdict.name);
    REQUIRE(golden.isMember(verdict.name));
    CHECK(golden[verdict.name] == verdict.response);
  }
}
