#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <controllers/health-controller.hxx>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <proxy/proxy-config.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/api-response/api-response.hxx>
#include <proxy/reverse-proxy.hxx>
#include <sync/camera-fan-out.hxx>
#include <sync/sync-fan-out.hxx>
#include <sync/sync-registrar.hxx>
#include <sync/sync-relay.hxx>

#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace
{

Json::Value parseBody(const drogon::HttpResponsePtr& response)
{
  Json::Value body;
  Json::Reader reader;
  const std::string text(response->getBody().begin(),
                         response->getBody().end());
  CHECK(reader.parse(text, body));
  return body;
}

} // namespace

TEST_CASE("health envelope conforms to the ApiResponse shape")
{
  auto response = ApiResponse::ok(HealthController::info(12.5));

  const Json::Value body = parseBody(response);

  CHECK(body["status"].isInt());
  CHECK(body["status"].asInt() == 200);
  CHECK(body["info"]["service"] == "argus-gateway");
  CHECK(body["info"]["uptimeSeconds"].asDouble() == doctest::Approx(12.5));
  CHECK(body.isMember("errors"));
  CHECK(response->getStatusCode() == drogon::k200OK);
  CHECK(response->getContentType() == drogon::CT_APPLICATION_JSON);
}

TEST_CASE("identity config resolves database and schema with defaults")
{
  const char* path = "gateway-test-config-empty.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const IdentityDbConfig config = IdentityConfig::resolveDb();

  CHECK(config.dbPath == "database/identity.db");
  CHECK(config.schemaPath == "database/identity-schema.sql");

  std::remove(path);
}

TEST_CASE("identity config honors the identity section overrides")
{
  const char* path = "gateway-test-config-identity.toml";
  {
    std::ofstream file(path);
    file << "[identity]\n"
         << "db = \"/tmp/argus-test/identity.db\"\n"
         << "schema = \"/tmp/argus-test/identity-schema.sql\"\n";
  }

  ConfigService::load(path);
  const IdentityDbConfig config = IdentityConfig::resolveDb();

  CHECK(config.dbPath == "/tmp/argus-test/identity.db");
  CHECK(config.schemaPath == "/tmp/argus-test/identity-schema.sql");

  std::remove(path);
}

TEST_CASE("runtime override wins over the config file value")
{
  const char* path = "gateway-test-config-runtime.toml";
  {
    std::ofstream file(path);
    file << "[database]\n"
         << "file = \"database/argus.db\"\n";
  }

  ConfigService::load(path);
  CHECK(ConfigService::getString("database.file") == "database/argus.db");

  ConfigService::setRuntimeString("database.file", "database/identity.db");
  CHECK(ConfigService::getString("database.file") == "database/identity.db");

  std::remove(path);
}

TEST_CASE("identity surface registers controllers and filters once")
{
  const char* path = "gateway-test-config-register.toml";
  {
    std::ofstream file(path);
    file << "[jwt]\n"
         << "secret = \"0123456789abcdef0123456789abcdef0123456789\"\n"
         << "refresh_secret = \"fedcba9876543210fedcba9876543210fedcba98\"\n"
         << "access_ttl_minutes = 15\n"
         << "refresh_ttl_days = 30\n";
  }

  ConfigService::load(path);
  std::remove(path);

  const IdentityRegistrationStats stats = registerIdentitySurface();

  CHECK(stats.controllers == 5);
  CHECK(stats.filters == 4);
}

TEST_CASE("gateway config section resolves listener and nats keys")
{
  const char* path = "gateway-test-config.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n"
         << "host = \"127.0.0.1\"\n"
         << "\n"
         << "[nats]\n"
         << "url = \"\"\n";
  }

  ConfigService::load(path);

  CHECK(ConfigService::getInt("gateway.port") == 7024);
  CHECK(ConfigService::getString("gateway.host") == "127.0.0.1");
  CHECK(ConfigService::getString("nats.url").empty());

  std::remove(path);
}

TEST_CASE("legacy config resolves the relay url and database with defaults")
{
  const char* path = "gateway-test-config-legacy-empty.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const LegacySyncConfig config = LegacySyncConfig::resolve();

  CHECK(config.syncUrl.empty());
  CHECK(config.dbPath == "database/argus.db");

  std::remove(path);
}

TEST_CASE("legacy config honors the legacy section overrides")
{
  const char* path = "gateway-test-config-legacy.toml";
  {
    std::ofstream file(path);
    file << "[legacy]\n"
         << "sync_url = \"wss://127.0.0.1:7024\"\n"
         << "db = \"/tmp/argus-test/argus.db\"\n";
  }

  ConfigService::load(path);
  const LegacySyncConfig config = LegacySyncConfig::resolve();

  CHECK(config.syncUrl == "wss://127.0.0.1:7024");
  CHECK(config.dbPath == "/tmp/argus-test/argus.db");

  std::remove(path);
}

TEST_CASE("relay text allowlist keeps every camera and voice frame type")
{
  struct Row
  {
    const char* type;
    bool allowed;
  };
  static const Row table[] = {
      // Legacy camera emissions and errors
      {"camera:ready", true},
      {"camera:closed", true},
      {"camera:subscribe_error", true},
      {"camera:ack_error", true},
      {"camera:unsubscribe_error", true},
      // Voice emissions, session control and errors
      {"voice:event", true},
      {"voice:done", true},
      {"voice:stt", true},
      {"voice:assistant", true},
      {"voice:start_error", true},
      {"voice:stop_error", true},
      {"voice:skip_error", true},
      // Never forwarded: the gateway serves the sync protocol natively
      {"sync", false},
      {"sync_audit_log", false},
      {"sync_user_audit_log", false},
      {"initial_info", false},
      {"add", false},
      {"delete", false},
      {"log", false},
      {"auth_context_changed", false},
      {"unknown", false},
      {"", false},
  };

  for (const auto& row : table)
    CHECK(relayAllowedText(row.type) == row.allowed);
}

TEST_CASE("fan-out parses the sync-change wire contract")
{
  const Json::Value moduleEmit = json_util::fromString(
      R"({"operation":4,"option":"camera","info":{"id":3}})");
  const auto module = sync_fan_out::parseEvent(moduleEmit);
  REQUIRE(module);
  CHECK(module->emit.operation == SyncOperation::Add);
  CHECK(module->users == std::nullopt);
  CHECK(moduleRoom(module->emit.option) == moduleRoom(TableName::Camera));

  const Json::Value userEmit = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[42,43]})");
  const auto user = sync_fan_out::parseEvent(userEmit);
  REQUIRE(user);
  REQUIRE(user->users);
  CHECK(user->users->size() == 2);
  CHECK(userRoom((*user->users)[0]) == userRoom(42));

  const Json::Value userEmitEmpty = json_util::fromString(
      R"({"operation":4,"option":"user","info":{},"users":[]})");
  const auto empty = sync_fan_out::parseEvent(userEmitEmpty);
  REQUIRE(empty);
  REQUIRE(empty->users);
  CHECK(empty->users->empty());
  // An explicit empty users list must not fall back to the module room.
  CHECK(empty->user == std::nullopt);

  const Json::Value disconnect = json_util::fromString(
      R"({"operation":7,"option":"user","info":{},"users":[7],"user":7,"action":"disconnect"})");
  const auto disconnection = sync_fan_out::parseEvent(disconnect);
  REQUIRE(disconnection);
  CHECK(disconnection->user == 7);

  // The room-control action must round-trip the payload the legacy actually
  // publishes, not a hand-written shape.
  RoleRoomReplaceInput input;
  input.userId = 7;
  input.oldRole = UserRole::Resident;
  input.newRole = UserRole::Guest;
  const auto replacement = sync_fan_out::parseEvent(
      sync_change::roleRoomsPayload(input));
  REQUIRE(replacement);
  CHECK(replacement->emit.operation == SyncOperation::AuthContextChanged);
  CHECK(replacement->emit.option == TableName::User);
  CHECK(replacement->user == 7);
  CHECK(replacement->oldRole == UserRole::Resident);
  CHECK(replacement->newRole == UserRole::Guest);

  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString(
      R"({"option":"camera","info":{}})")));
  CHECK_FALSE(sync_fan_out::parseEvent(json_util::fromString("null")));
  CHECK_FALSE(sync_fan_out::parseEvent(
      json_util::fromString(R"({"action":"disconnect","operation":7,"option":"user","info":{}})")));
}

TEST_CASE("fan-out routing table matches the legacy SocketService mapping")
{
  const auto emit = [](SyncOperation operation, TableName table) {
    SocketEmitDto body;
    body.operation = operation;
    body.option = table;
    body.obj["id"] = 7;
    return body;
  };

  // Absent users: the module room of the table.
  const auto module =
      sync_fan_out::parseEvent(sync_change::emitPayload(emit(SyncOperation::Add,
                                                             TableName::Camera)));
  REQUIRE(module);
  const sync_fan_out::FanOutPlan modulePlan = sync_fan_out::planEvent(*module);
  CHECK(modulePlan.kind == sync_fan_out::FanOutPlan::Kind::ModuleEmit);
  CHECK(modulePlan.room == moduleRoom(TableName::Camera));
  CHECK(modulePlan.rooms.empty());

  // Explicit users: the user rooms of the ids, never the module room.
  const auto scoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {42, 43}));
  REQUIRE(scoped);
  const sync_fan_out::FanOutPlan scopedPlan = sync_fan_out::planEvent(*scoped);
  CHECK(scopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  REQUIRE(scopedPlan.rooms.size() == 2);
  CHECK(scopedPlan.rooms[0] == userRoom(42));
  CHECK(scopedPlan.rooms[1] == userRoom(43));

  // Explicit empty users: user emit with no rooms, no module-room fallback.
  const auto unscoped =
      sync_fan_out::parseEvent(sync_change::userEmitPayload(
          emit(SyncOperation::Add, TableName::Notification), {}));
  REQUIRE(unscoped);
  const sync_fan_out::FanOutPlan unscopedPlan = sync_fan_out::planEvent(*unscoped);
  CHECK(unscopedPlan.kind == sync_fan_out::FanOutPlan::Kind::UserEmit);
  CHECK(unscopedPlan.rooms.empty());

  // Room-control actions take precedence over the emit fields; the
  // disconnect input is the user id, the role-rooms input is the same
  // RoleRoomReplaceInput the legacy replaceRoleRooms consumes.
  const auto disconnection =
      sync_fan_out::parseEvent(sync_change::disconnectPayload(
          emit(SyncOperation::AuthContextChanged, TableName::User), 42));
  REQUIRE(disconnection);
  const sync_fan_out::FanOutPlan disconnectPlan = sync_fan_out::planEvent(*disconnection);
  CHECK(disconnectPlan.kind == sync_fan_out::FanOutPlan::Kind::Disconnect);
  CHECK(disconnectPlan.userId == 42);

  RoleRoomReplaceInput input;
  input.userId = 42;
  input.oldRole = UserRole::Resident;
  input.newRole = UserRole::Guest;
  const auto replacement =
      sync_fan_out::parseEvent(sync_change::roleRoomsPayload(input));
  REQUIRE(replacement);
  const sync_fan_out::FanOutPlan replacePlan = sync_fan_out::planEvent(*replacement);
  CHECK(replacePlan.kind == sync_fan_out::FanOutPlan::Kind::ReplaceRoleRooms);
  CHECK(replacePlan.replaceInput.userId == 42);
  CHECK(replacePlan.replaceInput.oldRole == UserRole::Resident);
  CHECK(replacePlan.replaceInput.newRole == UserRole::Guest);
}

TEST_CASE("fan-out re-emits the exact legacy wire triple")
{
  const Json::Value payload = json_util::fromString(
      R"({"operation":5,"option":"reminder","info":{"id":9,"title":"x"},"users":[]})");
  const auto event = sync_fan_out::parseEvent(payload);
  REQUIRE(event);

  SocketEmitDto body;
  body.operation = SyncOperation::Delete;
  body.option = TableName::Reminder;
  Json::Value info;
  info["id"] = 9;
  info["title"] = "x";
  body.obj = info;

  CHECK(json_util::toString(event->emit.toJson()) ==
        json_util::toString(body.toJson()));
}

TEST_CASE("read-only legacy database rejects writes and serves reads")
{
  const char* dbPath = "gateway-test-readonly.db";
  std::remove(dbPath);

  DbService::enableUriFilenames();
  auto writable = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=") + dbPath, 1);
  writable->execSqlSync(
      "CREATE TABLE note (id INTEGER PRIMARY KEY, text TEXT NOT NULL)");
  writable->execSqlSync("INSERT INTO note (text) VALUES ('hello')");
  writable.reset();

  const auto readOnly = drogon::orm::DbClient::newSqlite3Client(
      std::string("filename=file:") + dbPath + "?mode=ro", 1);
  DbService::setReadOnlyClient(readOnly);

  const auto rows = readOnly->execSqlSync("SELECT text FROM note");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["text"].as<std::string>() == "hello");

  bool writeFailed = false;
  try {
    readOnly->execSqlSync("INSERT INTO note (text) VALUES ('nope')");
  }
  catch (const std::exception&) {
    writeFailed = true;
  }
  CHECK(writeFailed);

  DbService::setReadOnlyClient(nullptr);
  std::remove(dbPath);
}

TEST_CASE("sync surface registers the socket with the relay forwarder")
{
  const char* path = "gateway-test-config-sync.toml";
  {
    std::ofstream file(path);
    file << "[jwt]\n"
         << "secret = \"0123456789abcdef0123456789abcdef0123456789\"\n"
         << "refresh_secret = \"fedcba9876543210fedcba9876543210fedcba98\"\n"
         << "access_ttl_minutes = 15\n"
         << "refresh_ttl_days = 30\n";
  }

  ConfigService::load(path);
  std::remove(path);

  const SyncRegistrationStats stats = registerSyncSurface(nullptr);

  CHECK(stats.controllers == 1);
  CHECK(stats.filters == 2);
}

TEST_CASE("listener config resolves the cutover TLS listener by default")
{
  const char* path = "gateway-test-config-listener.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n"
         << "\n"
         << "[cert]\n"
         << "server_cert = \"certs/server.pem\"\n"
         << "server_key = \"certs/server.key\"\n";
  }

  ConfigService::load(path);
  const ListenerConfig config = ListenerConfig::resolve();
  const Json::Value listeners = listenerJson(config);

  CHECK(config.host == "0.0.0.0");
  CHECK(config.port == 7024);
  CHECK(config.tls);
  CHECK(config.certPath == "certs/server.pem");
  CHECK(config.keyPath == "certs/server.key");
  CHECK(config.minTlsProtocol == "TLSv1.2");
  CHECK(listeners.size() == 1);
  CHECK(listeners[0]["address"] == "0.0.0.0");
  CHECK(listeners[0]["port"].asInt() == 7024);
  CHECK(listeners[0]["https"].asBool() == true);
  CHECK(listeners[0]["cert"] == "certs/server.pem");
  CHECK(listeners[0]["key"] == "certs/server.key");
  REQUIRE(listeners[0]["ssl_conf"].size() == 1);
  CHECK(listeners[0]["ssl_conf"][0][0] == "MinProtocol");
  CHECK(listeners[0]["ssl_conf"][0][1] == "TLSv1.2");

  std::remove(path);
}

TEST_CASE("plain listener option serves local tests without TLS")
{
  const char* path = "gateway-test-config-plain.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "host = \"127.0.0.1\"\n"
         << "port = 7044\n"
         << "plain = true\n";
  }

  ConfigService::load(path);
  const ListenerConfig config = ListenerConfig::resolve();
  const Json::Value listeners = listenerJson(config);

  CHECK_FALSE(config.tls);
  CHECK(listeners[0]["https"].asBool() == false);
  CHECK_FALSE(listeners[0].isMember("cert"));
  CHECK_FALSE(listeners[0].isMember("ssl_conf"));

  std::remove(path);
}

TEST_CASE("proxy config resolves the internal upstream and native paths")
{
  const char* path = "gateway-test-config-proxy.toml";
  {
    std::ofstream file(path);
    file << "[legacy]\n"
         << "proxy_url = \"http://127.0.0.1:7025\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  CHECK(config.upstreamUrl == "http://127.0.0.1:7025");
  REQUIRE(config.exclusions.size() == 7);
  const std::vector<std::string> expected = {
      "/auth", "/invitation", "/pairing", "/portrait-preview",
      "/user", "/sync", "/health",
  };
  CHECK(config.exclusions == expected);

  ProxyConfig disabled;
  CHECK(disabled.upstreamUrl.empty());

  std::remove(path);
}

TEST_CASE("proxy config routes the camera CRUD to argus-camera")
{
  const char* path = "gateway-test-config-camera-proxy.toml";
  {
    std::ofstream file(path);
    file << "[legacy]\n"
         << "proxy_url = \"http://127.0.0.1:7025\"\n"
         << "[camera]\n"
         << "proxy_url = \"http://127.0.0.1:7026\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  CHECK(config.upstreamUrl == "http://127.0.0.1:7025");
  CHECK(config.cameraProxyUrl == "http://127.0.0.1:7026");

  // Without the [camera] section the camera routes stay on the legacy.
  {
    std::ofstream file(path);
    file << "[legacy]\n"
         << "proxy_url = \"http://127.0.0.1:7025\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig legacyOnly = ProxyConfig::resolve();
  CHECK(legacyOnly.cameraProxyUrl.empty());

  std::remove(path);
}

TEST_CASE("camera relay config resolves the sync target")
{
  const char* path = "gateway-test-config-camera-sync.toml";
  {
    std::ofstream file(path);
    file << "[camera]\n"
         << "sync_url = \"ws://127.0.0.1:7026/sync\"\n";
  }

  ConfigService::load(path);
  const CameraSyncConfig config = CameraSyncConfig::resolve();
  CHECK(config.syncUrl == "ws://127.0.0.1:7026/sync");

  {
    std::ofstream file(path);
    file << "[legacy]\n"
         << "proxy_url = \"http://127.0.0.1:7025\"\n";
  }

  ConfigService::load(path);
  const CameraSyncConfig fallback = CameraSyncConfig::resolve();
  CHECK(fallback.syncUrl.empty());

  std::remove(path);
}

TEST_CASE("relay leg routing sends camera frames to argus-camera")
{
  CHECK(relayLegIsCamera("camera:subscribe"));
  CHECK(relayLegIsCamera("camera:ready"));
  CHECK(relayLegIsCamera("camera:closed"));
  CHECK(relayLegIsCamera("camera:ack"));
  CHECK_FALSE(relayLegIsCamera("camera"));
  CHECK_FALSE(relayLegIsCamera("cameraX"));
  CHECK_FALSE(relayLegIsCamera("voice:transcribe"));

  // Every camera frame type the legacy emits routes to the camera leg.
  for (const char* type :
       {"camera:subscribe", "camera:ready", "camera:closed", "camera:ack",
        "camera:subscribe_error", "camera:ack_error",
        "camera:unsubscribe_error"})
    CHECK(relayLegIsCamera(type));
}

TEST_CASE("route table sends two-segment CRUD to the camera backend")
{
  gateway_proxy::SimpleReverseProxy proxy;
  Json::Value config;
  Json::Value backends(Json::arrayValue);
  backends.append("http://127.0.0.1:7025");
  config["backends"] = backends;
  Json::Value routes(Json::arrayValue);
  Json::Value cameraRoute(Json::objectValue);
  Json::Value prefixes(Json::arrayValue);
  prefixes.append("/camera");
  prefixes.append("/zone");
  cameraRoute["prefixes"] = prefixes;
  cameraRoute["max_segments"] = 2;
  cameraRoute["backend"] = "http://127.0.0.1:7026";
  routes.append(cameraRoute);
  config["routes"] = routes;

  // initAndStart registers the pre-routing advice; a plugin instance is
  // started once per process, so this case runs alone here.
  proxy.initAndStart(config);

  CHECK(proxy.matchRoute("/camera") == 0);
  CHECK(proxy.matchRoute("/camera/1") == 0);
  CHECK(proxy.matchRoute("/zone") == 0);
  CHECK(proxy.matchRoute("/zone/3") == 0);

  // The deeper control paths miss the segment cap and stay with the legacy.
  CHECK(proxy.matchRoute("/camera/1/ptz") == -1);
  CHECK(proxy.matchRoute("/camera/1/preset") == -1);
  CHECK(proxy.matchRoute("/camera/1/settings") == -1);
  CHECK(proxy.matchRoute("/camera/1/settings/x") == -1);
  CHECK(proxy.matchRoute("/camera/1/status") == -1);
  CHECK(proxy.matchRoute("/camera/1/presets") == -1);
  CHECK(proxy.matchRoute("/camera/1/capabilities") == -1);
  CHECK(proxy.matchRoute("/camera/1/talk") == -1);
  CHECK(proxy.matchRoute("/cameras/1") == -1);
  CHECK(proxy.matchRoute("/user/1") == -1);

  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera") == 1);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1") == 2);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1/ptz") == 3);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1//x") == 3);

  proxy.shutdown();
}

TEST_CASE("native path match keeps segment boundaries")
{
  const std::vector<std::string> exclusions = {"/user", "/sync"};

  CHECK(isGatewayNativePath("/user", exclusions));
  CHECK(isGatewayNativePath("/user/1", exclusions));
  CHECK(isGatewayNativePath("/sync", exclusions));
  CHECK_FALSE(isGatewayNativePath("/userx", exclusions));
  CHECK_FALSE(isGatewayNativePath("/camera", exclusions));
  CHECK_FALSE(isGatewayNativePath("/camera/1/status", exclusions));
}

TEST_CASE("proxy exclusion set covers every registered gateway route")
{
  const char* path = "gateway-test-config-coverage.toml";
  {
    std::ofstream file(path);
    file << "[jwt]\n"
         << "secret = \"0123456789abcdef0123456789abcdef0123456789\"\n"
         << "refresh_secret = \"fedcba9876543210fedcba9876543210fedcba98\"\n";
  }

  ConfigService::load(path);
  std::remove(path);

  drogon::app().registerController(std::make_shared<HealthController>());
  registerIdentitySurface();
  registerSyncSurface(nullptr);

  const ProxyConfig proxy = ProxyConfig::resolve();
  REQUIRE_FALSE(proxy.exclusions.empty());

  for (const auto& handlerInfo : drogon::app().getHandlersInfo()) {
    const auto& pattern = std::get<0>(handlerInfo);
    if (pattern.empty() || pattern.front() != '/')
      continue;
    CHECK_MESSAGE(isGatewayNativePath(pattern, proxy.exclusions),
                  pattern);
  }
}
