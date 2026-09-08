#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <filter/device/device-filter.hxx>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <proxy/proxy-config.hxx>
#include <server/listener-config.hxx>
#include <server/refresh-rate-limiter.hxx>
#include <server/remote-config.hxx>
#include <server/remote-gate.hxx>
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
#include <sync/voice-grpc-relay.hxx>

#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

// Recording stub of a relay leg: counts lifecycle callbacks, keeps forwarded frames.
class RecordingLeg final : public SyncForwarder
{
public:
  void onConnect(const drogon::HttpRequestPtr&,
                 const drogon::WebSocketConnectionPtr&) override
  {
    ++connects;
  }

  drogon::Task<bool> forwardText(const drogon::WebSocketConnectionPtr&,
                                 const Json::Value&, std::string_view raw)
      override
  {
    texts.emplace_back(raw);
    co_return true;
  }

  void forwardBinary(const drogon::WebSocketConnectionPtr&,
                     const std::string& data) override
  {
    binaries.push_back(data);
  }

  void onClose(const drogon::WebSocketConnectionPtr&) override { ++closes; }

  int connects{0};
  int closes{0};
  std::vector<std::string> texts;
  std::vector<std::string> binaries;
};

Json::Value parseBody(const drogon::HttpResponsePtr& response)
{
  Json::Value body;
  Json::Reader reader;
  const std::string text(response->getBody().begin(),
                         response->getBody().end());
  CHECK(reader.parse(text, body));
  return body;
}

drogon::HttpRequestPtr testRequest(drogon::HttpMethod method,
                                   const std::string& path)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method);
  req->setPath(path);
  req->addHeader("User-Agent", "gateway-test");
  return req;
}

} // namespace

TEST_CASE("health envelope conforms to the ApiResponse shape")
{
  auto response = ApiResponse::ok(HealthController::info("argus-gateway", 12.5));

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
  CHECK(config.schemaPath == "argus-identity/database/schema.sql");

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

TEST_CASE("identity change events never fan out to the client sockets")
{
  // argus.identity.v1.change rides the sync wildcard, but its wire contract
  // is an identity row diff, not a sync-change: the gateway must not be able
  // to forward it to the /user rooms (Ruling BX — the replicas are the only
  // consumer).
  const Json::Value identity = json_util::fromString(
      R"({"kind":"identity","table":"person","id":7,"deleted":false,
          "row":{"id":7,"user_id":42,"name":"Ana Garcia"}})");
  CHECK(sync_fan_out::parseEvent(identity) == std::nullopt);

  const Json::Value tombstone = json_util::fromString(
      R"({"kind":"identity","table":"person","id":7,"deleted":true,
          "row":{}})");
  CHECK(sync_fan_out::parseEvent(tombstone) == std::nullopt);
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

  const SyncRegistrationStats stats = registerSyncSurface(nullptr, nullptr);

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
  const ListenerConfig config = ListenerConfig::resolveTls(7024);
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
  const ListenerConfig config = ListenerConfig::resolveTls(7024);
  const Json::Value listeners = listenerJson(config);

  CHECK_FALSE(config.tls);
  CHECK(listeners[0]["https"].asBool() == false);
  CHECK_FALSE(listeners[0].isMember("cert"));
  CHECK_FALSE(listeners[0].isMember("ssl_conf"));

  std::remove(path);
}

TEST_CASE("proxy config resolves the native paths")
{
  const char* path = "gateway-test-config-proxy.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  REQUIRE(config.exclusions.size() == 7);
  const std::vector<std::string> expected = {
      "/auth", "/invitation", "/pairing", "/portrait-preview",
      "/user", "/sync", "/health",
  };
  CHECK(config.exclusions == expected);
  CHECK(config.cameraProxyUrl.empty());
  CHECK(config.productivityProxyUrl.empty());
  CHECK(config.notificationProxyUrl.empty());

  std::remove(path);
}

TEST_CASE("proxy config routes the camera CRUD to argus-camera")
{
  const char* path = "gateway-test-config-camera-proxy.toml";
  {
    std::ofstream file(path);
    file << "[camera]\n"
         << "proxy_url = \"http://127.0.0.1:7026\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  CHECK(config.cameraProxyUrl == "http://127.0.0.1:7026");

  // Without the [camera] section the camera routes stay unrouted.
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const ProxyConfig unrouted = ProxyConfig::resolve();
  CHECK(unrouted.cameraProxyUrl.empty());

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
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const CameraSyncConfig fallback = CameraSyncConfig::resolve();
  CHECK(fallback.syncUrl.empty());

  std::remove(path);
}

TEST_CASE("voice gRPC config resolves the typed voice leg target")
{
  const char* path = "gateway-test-config-voice-grpc.toml";
  {
    std::ofstream file(path);
    file << "[voice]\n"
         << "target = \"127.0.0.1:7034\"\n";
  }

  ConfigService::load(path);
  const VoiceGrpcConfig cutover = VoiceGrpcConfig::resolve();
  CHECK(cutover.target == "127.0.0.1:7034");

  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const VoiceGrpcConfig fallback = VoiceGrpcConfig::resolve();
  CHECK(fallback.target.empty());

  std::remove(path);
}

TEST_CASE("renderServerFrame reproduces the frozen voice wire JSON")
{
  argus::voice::v1::ServerFrame stt;
  stt.mutable_stt()->set_text("hola argus");
  stt.mutable_stt()->set_final(true);
  const Json::Value sttJson =
      json_util::fromString(json_util::toString(
          VoiceGrpcRelay::renderServerFrame(stt)));
  CHECK(sttJson["type"] == "voice:stt");
  CHECK(sttJson["payload"]["text"] == "hola argus");
  CHECK(sttJson["payload"]["final"] == true);

  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text("Hola de nuevo.");
  const Json::Value assistantJson =
      json_util::fromString(json_util::toString(
          VoiceGrpcRelay::renderServerFrame(assistant)));
  CHECK(assistantJson["type"] == "voice:assistant");
  CHECK(assistantJson["payload"]["text"] == "Hola de nuevo.");

  argus::voice::v1::ServerFrame event;
  event.mutable_event()->set_reaction(
      argus::voice::v1::REACTION_RECOGNIZING);
  event.mutable_event()->set_intensity(0.5F);
  event.mutable_event()->set_because("stt_failed");
  const Json::Value eventJson =
      json_util::fromString(json_util::toString(
          VoiceGrpcRelay::renderServerFrame(event)));
  CHECK(eventJson["type"] == "voice:event");
  CHECK(eventJson["payload"]["reaction"] == "recognizing");
  CHECK(eventJson["payload"]["intensity"].asDouble() ==
        doctest::Approx(0.5));
  CHECK(eventJson["payload"]["because"] == "stt_failed");

  argus::voice::v1::ServerFrame done;
  done.mutable_done()->set_session_id(0);
  const Json::Value doneJson =
      json_util::fromString(json_util::toString(
          VoiceGrpcRelay::renderServerFrame(done)));
  CHECK(doneJson["type"] == "voice:done");
  CHECK(doneJson["payload"]["sessionId"].asInt64() == 0);
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

TEST_CASE("relay leg routing sends voice frames to argus-voice")
{
  CHECK(relayLegIsVoice("voice:start"));
  CHECK(relayLegIsVoice("voice:stop"));
  CHECK(relayLegIsVoice("voice:skip"));
  CHECK_FALSE(relayLegIsVoice("voice"));
  CHECK_FALSE(relayLegIsVoice("voiceX"));
  CHECK_FALSE(relayLegIsVoice("camera:subscribe"));

  // Every voice frame type the legacy emits routes to the voice leg.
  for (const char* type :
       {"voice:start", "voice:stop", "voice:skip", "voice:stt",
        "voice:assistant", "voice:event", "voice:done", "voice:start_error",
        "voice:stop_error", "voice:skip_error"})
    CHECK(relayLegIsVoice(type));
}

TEST_CASE("composite relay split routes voice frames and binary to the "
          "voice leg")
{
  auto camera = std::make_shared<RecordingLeg>();
  auto voice = std::make_shared<RecordingLeg>();
  CompositeSyncRelay relay(camera, voice);

  relay.onConnect(nullptr, nullptr);
  CHECK(camera->connects == 1);
  CHECK(voice->connects == 1);

  const drogon::WebSocketConnectionPtr conn;
  Json::Value cameraFrame;
  cameraFrame["type"] = "camera:subscribe";
  CHECK(drogon::sync_wait(relay.forwardText(
      conn, cameraFrame, "{\"type\":\"camera:subscribe\"}")));

  Json::Value voiceFrame;
  voiceFrame["type"] = "voice:start";
  CHECK(drogon::sync_wait(relay.forwardText(
      conn, voiceFrame, "{\"type\":\"voice:start\"}")));

  relay.forwardBinary(conn, std::string("\x01\x02\x03", 3));

  CHECK(camera->texts.size() == 1);
  CHECK(camera->texts.front() == "{\"type\":\"camera:subscribe\"}");
  CHECK(voice->texts.size() == 1);
  CHECK(voice->texts.front() == "{\"type\":\"voice:start\"}");
  CHECK(camera->binaries.empty());
  CHECK(voice->binaries.size() == 1);
  CHECK(voice->binaries.front() == std::string("\x01\x02\x03", 3));

  relay.onClose(nullptr);
  CHECK(camera->closes == 1);
  CHECK(voice->closes == 1);
}

TEST_CASE("route table sends the whole camera domain to the camera backend")
{
  gateway_proxy::SimpleReverseProxy proxy;
  Json::Value config;
  Json::Value routes(Json::arrayValue);
  Json::Value cameraRoute(Json::objectValue);
  Json::Value prefixes(Json::arrayValue);
  prefixes.append("/camera");
  prefixes.append("/zone");
  cameraRoute["prefixes"] = prefixes;
  cameraRoute["max_segments"] = 8;
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

  // The device-control paths ride the same route now that the cap spans
  // the whole domain.
  CHECK(proxy.matchRoute("/camera/1/ptz") == 0);
  CHECK(proxy.matchRoute("/camera/1/preset") == 0);
  CHECK(proxy.matchRoute("/camera/1/settings") == 0);
  CHECK(proxy.matchRoute("/camera/1/status") == 0);
  CHECK(proxy.matchRoute("/camera/1/presets") == 0);
  CHECK(proxy.matchRoute("/camera/1/capabilities") == 0);
  CHECK(proxy.matchRoute("/camera/1/talk") == 0);
  // Beyond the cap and foreign prefixes still fall through.
  CHECK(proxy.matchRoute("/camera/1/settings/x/y/z") == 0);
  CHECK(proxy.matchRoute("/camera/1/a/b/c/d/e/f/g/h/i") == -1);
  CHECK(proxy.matchRoute("/cameras/1") == -1);
  CHECK(proxy.matchRoute("/user/1") == -1);

  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera") == 1);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1") == 2);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1/ptz") == 3);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount("/camera/1//x") == 3);

  proxy.shutdown();
}

TEST_CASE("proxy config routes the productivity and notification domains")
{
  const char* path = "gateway-test-config-f32-proxy.toml";
  {
    std::ofstream file(path);
    file << "[productivity]\n"
         << "proxy_url = \"http://127.0.0.1:7027\"\n"
         << "[notifications]\n"
         << "proxy_url = \"http://127.0.0.1:7028\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  CHECK(config.productivityProxyUrl == "http://127.0.0.1:7027");
  CHECK(config.notificationProxyUrl == "http://127.0.0.1:7028");

  // Without the sections the domains stay unrouted.
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const ProxyConfig unrouted = ProxyConfig::resolve();
  CHECK(unrouted.productivityProxyUrl.empty());
  CHECK(unrouted.notificationProxyUrl.empty());

  std::remove(path);
}

TEST_CASE("route table sends the productivity and notification domains to the fase 3 backends")
{
  gateway_proxy::SimpleReverseProxy proxy;
  Json::Value config;
  Json::Value routes(Json::arrayValue);

  Json::Value productivityRoute(Json::objectValue);
  Json::Value productivityPrefixes(Json::arrayValue);
  productivityPrefixes.append("/calendar-event");
  productivityPrefixes.append("/calendar-event-share");
  productivityPrefixes.append("/project");
  productivityPrefixes.append("/project-member");
  productivityPrefixes.append("/project-task");
  productivityRoute["prefixes"] = productivityPrefixes;
  productivityRoute["max_segments"] = 8;
  productivityRoute["backend"] = "http://127.0.0.1:7027";
  routes.append(productivityRoute);

  Json::Value notificationRoute(Json::objectValue);
  Json::Value notificationPrefixes(Json::arrayValue);
  notificationPrefixes.append("/notification");
  notificationPrefixes.append("/notification-token");
  notificationRoute["prefixes"] = notificationPrefixes;
  notificationRoute["max_segments"] = 2;
  notificationRoute["backend"] = "http://127.0.0.1:7028";
  routes.append(notificationRoute);

  config["routes"] = routes;

  proxy.initAndStart(config);

  // Every method and subpath of the productivity domain routes through; the
  // segment-boundary match keeps /calendar-event-share distinct from
  // /calendar-event.
  CHECK(proxy.matchRoute("/calendar-event") == 0);
  CHECK(proxy.matchRoute("/calendar-event/1") == 0);
  CHECK(proxy.matchRoute("/calendar-event-share") == 0);
  CHECK(proxy.matchRoute("/calendar-event-share/1") == 0);
  CHECK(proxy.matchRoute("/project") == 0);
  CHECK(proxy.matchRoute("/project/1") == 0);
  CHECK(proxy.matchRoute("/project-member/1") == 0);
  CHECK(proxy.matchRoute("/project-task/1") == 0);
  CHECK(proxy.matchRoute("/calendar-event/1/a/b/c/d/e/f") == 0);
  CHECK(proxy.matchRoute("/calendar-event/1/a/b/c/d/e/f/g") == -1);

  CHECK(proxy.matchRoute("/notification") == 1);
  CHECK(proxy.matchRoute("/notification/read") == 1);
  CHECK(proxy.matchRoute("/notification-token") == 1);
  CHECK(proxy.matchRoute("/notification/read/1") == -1);
  CHECK(proxy.matchRoute("/notifications/1") == -1);
  CHECK(proxy.matchRoute("/notificationtoken") == -1);

  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount(
            "/calendar-event/1/a/b/c/d/e/f") == 8);
  CHECK(gateway_proxy::SimpleReverseProxy::segmentCount(
            "/calendar-event/1/a/b/c/d/e/f/g") == 9);

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

  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-gateway"}));
  registerIdentitySurface();
  registerSyncSurface(nullptr, nullptr);

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

TEST_CASE("remote config resolves disabled by default and honors overrides")
{
  const char* path = "gateway-test-config-remote-default.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const RemoteConfig config = RemoteConfig::resolve();

  CHECK(config.tunnelPort == 0);
  CHECK_FALSE(config.enabled);

  std::remove(path);

  const char* overrides = "gateway-test-config-remote.toml";
  {
    std::ofstream file(overrides);
    file << "[remote]\n"
         << "tunnel_port = 17443\n"
         << "enabled = true\n";
  }

  ConfigService::load(overrides);
  const RemoteConfig enabled = RemoteConfig::resolve();

  CHECK(enabled.tunnelPort == 17443);
  CHECK(enabled.enabled);

  std::remove(overrides);

  const char* invalid = "gateway-test-config-remote-invalid.toml";
  {
    std::ofstream file(invalid);
    file << "[remote]\n"
         << "tunnel_port = 70000\n";
  }

  ConfigService::load(invalid);
  const RemoteConfig rejected = RemoteConfig::resolve();

  CHECK(rejected.tunnelPort == 0);

  std::remove(invalid);
}

TEST_CASE("remote listener appends the tunnel listener only when configured")
{
  const char* path = "gateway-test-config-remote-listener.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "host = \"127.0.0.1\"\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const ListenerConfig base = ListenerConfig::resolveTls(7024);
  const RemoteConfig disabled;

  Json::Value listeners = listenerJson(base);
  appendRemoteListener(listeners, disabled, base);
  CHECK(listeners.size() == 1);

  RemoteConfig enabled;
  enabled.tunnelPort = 17443;
  listeners = listenerJson(base);
  appendRemoteListener(listeners, enabled, base);
  REQUIRE(listeners.size() == 2);
  CHECK(listeners[1]["address"] == base.host);
  CHECK(listeners[1]["port"].asInt() == 17443);
  CHECK(listeners[1]["https"].asBool() == base.tls);
  CHECK(listeners[1]["cert"] == base.certPath);
  CHECK(listeners[1]["key"] == base.keyPath);
  CHECK(listeners[1]["ssl_conf"][0][0] == "MinProtocol");
  CHECK(listeners[1]["ssl_conf"][0][1] == base.minTlsProtocol);

  std::remove(path);

  const char* plain = "gateway-test-config-remote-plain.toml";
  {
    std::ofstream file(plain);
    file << "[gateway]\n"
         << "port = 7044\n"
         << "plain = true\n";
  }

  ConfigService::load(plain);
  const ListenerConfig plainBase = ListenerConfig::resolveTls(7024);
  Json::Value plainListeners = listenerJson(plainBase);
  appendRemoteListener(plainListeners, enabled, plainBase);
  REQUIRE(plainListeners.size() == 2);
  CHECK(plainListeners[1]["https"].asBool() == false);
  CHECK_FALSE(plainListeners[1].isMember("cert"));
  CHECK_FALSE(plainListeners[1].isMember("ssl_conf"));

  std::remove(plain);
}

TEST_CASE("tunnel port validation refuses a gateway-port collision")
{
  const ListenerConfig base{.host = "0.0.0.0",
                            .port = 7024,
                            .tls = false,
                            .certPath = {},
                            .keyPath = {},
                            .minTlsProtocol = {}};

  RemoteConfig disabled;
  CHECK_NOTHROW(requireDistinctTunnelPort(base, disabled));

  RemoteConfig tunnel;
  tunnel.tunnelPort = 17443;
  CHECK_NOTHROW(requireDistinctTunnelPort(base, tunnel));

  RemoteConfig collision;
  collision.tunnelPort = 7024;
  CHECK_THROWS_AS(requireDistinctTunnelPort(base, collision),
                  std::runtime_error);
}

TEST_CASE("request classification stays local while the listener is disabled")
{
  RemoteConfig disabled;
  CHECK_FALSE(requestIsRemote(testRequest(drogon::Get, "/health"), disabled));

  RemoteConfig enabled;
  enabled.tunnelPort = 17443;
  CHECK_FALSE(requestIsRemote(testRequest(drogon::Get, "/health"), enabled));
}

TEST_CASE("remote gate rejects pairing and register for remote requests")
{
  const char* path = "gateway-test-config-remote-gate.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  std::remove(path);

  RemoteConfig config;
  config.tunnelPort = 17443;
  RemoteGate gate(
      config, std::make_shared<RefreshRateLimiter>(RateLimitConfig{}));

  auto pairing = testRequest(drogon::Post, "/pairing");
  const auto pairingResp = gate.check(pairing, true);
  const Json::Value body = parseBody(pairingResp);
  CHECK(body["status"].asInt() == 403);
  CHECK(body["errors"]["code"] == "REMOTE_NOT_ALLOWED");
  CHECK_FALSE(body["errors"]["message"].asString().empty());
  CHECK(body["info"].isNull());
  CHECK(body["errors"]["fields"].isNull());
  CHECK(pairingResp->getHeader("Access-Control-Allow-Origin") == "*");

  auto registration = testRequest(drogon::Post, "/auth/register");
  const Json::Value registerBody = parseBody(gate.check(registration, true));
  CHECK(registerBody["status"].asInt() == 403);
  CHECK(registerBody["errors"]["code"] == "REMOTE_NOT_ALLOWED");

  CHECK_FALSE(gate.check(testRequest(drogon::Post, "/pairing"), false));
  CHECK_FALSE(gate.check(testRequest(drogon::Get, "/health"), true));

  config.enabled = true;
  RemoteGate openGate(
      config, std::make_shared<RefreshRateLimiter>(RateLimitConfig{}));
  CHECK_FALSE(openGate.check(testRequest(drogon::Post, "/pairing"), true));
  CHECK_FALSE(openGate.check(testRequest(drogon::Post, "/auth/register"),
                             true));
}

TEST_CASE("remote gate marks tunnel requests with the remote attribute")
{
  RemoteConfig config;
  config.tunnelPort = 17443;
  RemoteGate gate(
      config, std::make_shared<RefreshRateLimiter>(RateLimitConfig{}));

  auto sync = testRequest(drogon::Get, "/sync");
  CHECK_FALSE(gate.check(sync, true));
  CHECK(sync->getAttributes()->find(AppConfig::REMOTE_CTX_KEY));
  CHECK(sync->getAttributes()->get<bool>(AppConfig::REMOTE_CTX_KEY));

  auto local = testRequest(drogon::Get, "/sync");
  CHECK_FALSE(gate.check(local, false));
  CHECK_FALSE(local->getAttributes()->find(AppConfig::REMOTE_CTX_KEY));
}

TEST_CASE("rate limit config resolves defaults and honors overrides")
{
  const char* path = "gateway-test-config-ratelimit-default.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const RateLimitConfig config = RateLimitConfig::resolve();

  CHECK_FALSE(config.enabled);
  CHECK(config.windowSeconds == 60);
  CHECK(config.maxRequests == 10);
  CHECK(config.lockoutThreshold == 5);
  CHECK(config.lockoutSeconds == 300);

  std::remove(path);

  const char* overrides = "gateway-test-config-ratelimit.toml";
  {
    std::ofstream file(overrides);
    file << "[rate_limit]\n"
         << "enabled = true\n"
         << "window_seconds = 30\n"
         << "max_requests = 3\n"
         << "lockout_threshold = 2\n"
         << "lockout_seconds = 120\n";
  }

  ConfigService::load(overrides);
  const RateLimitConfig custom = RateLimitConfig::resolve();

  CHECK(custom.enabled);
  CHECK(custom.windowSeconds == 30);
  CHECK(custom.maxRequests == 3);
  CHECK(custom.lockoutThreshold == 2);
  CHECK(custom.lockoutSeconds == 120);

  std::remove(overrides);

  const char* invalid = "gateway-test-config-ratelimit-invalid.toml";
  {
    std::ofstream file(invalid);
    file << "[rate_limit]\n"
         << "enabled = true\n"
         << "window_seconds = -5\n"
         << "max_requests = 0\n";
  }

  ConfigService::load(invalid);
  const RateLimitConfig guarded = RateLimitConfig::resolve();

  CHECK(guarded.windowSeconds == 60);
  CHECK(guarded.maxRequests == 10);

  std::remove(invalid);
}

TEST_CASE("rate limiter admits within the window and rejects past it")
{
  RateLimitConfig config;
  config.enabled = true;
  config.windowSeconds = 60;
  config.maxRequests = 2;
  RefreshRateLimiter limiter(config);

  const auto t0 = std::chrono::steady_clock::now();
  const auto second = std::chrono::seconds(1);

  CHECK(limiter.admit("key", t0));
  CHECK(limiter.admit("key", t0 + second));
  CHECK_FALSE(limiter.admit("key", t0 + 2 * second));
  CHECK(limiter.admit("other", t0));
  CHECK(limiter.admit("key", t0 + std::chrono::seconds(61)));
  CHECK(limiter.admit("key", t0 + std::chrono::seconds(62)));
  CHECK_FALSE(limiter.admit("key", t0 + std::chrono::seconds(63)));
}

TEST_CASE("disabled rate limiter never rejects")
{
  RefreshRateLimiter limiter(RateLimitConfig{});

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i)
    CHECK(limiter.admit("key", t0 + std::chrono::seconds(i)));

  RateLimitConfig failures;
  failures.enabled = false;
  RefreshRateLimiter failedLimiter(failures);
  CHECK_FALSE(failedLimiter.recordResult("key", false, t0));
}

TEST_CASE("rate limiter locks out after consecutive failures")
{
  RateLimitConfig config;
  config.enabled = true;
  config.maxRequests = 100;
  config.lockoutThreshold = 3;
  config.lockoutSeconds = 300;
  RefreshRateLimiter limiter(config);

  const auto t0 = std::chrono::steady_clock::now();
  const auto second = std::chrono::seconds(1);

  REQUIRE(limiter.admit("key", t0));
  CHECK_FALSE(limiter.recordResult("key", false, t0 + second));
  CHECK_FALSE(limiter.recordResult("key", false, t0 + 2 * second));
  CHECK(limiter.recordResult("key", false, t0 + 3 * second));

  CHECK_FALSE(limiter.admit("key", t0 + 4 * second));
  CHECK_FALSE(limiter.recordResult("key", false, t0 + 5 * second));

  CHECK_FALSE(limiter.admit("key", t0 + std::chrono::seconds(302)));
  CHECK(limiter.admit("key", t0 + std::chrono::seconds(304)));
  CHECK_FALSE(limiter.recordResult("key", true,
                                   t0 + std::chrono::seconds(305)));
  CHECK_FALSE(limiter.recordResult("key", false,
                                   t0 + std::chrono::seconds(306)));
  CHECK_FALSE(limiter.recordResult("key", false,
                                   t0 + std::chrono::seconds(307)));
}

TEST_CASE("remote gate rate limits refresh-token before routing")
{
  const char* path = "gateway-test-config-ratelimit-gate.toml";
  {
    std::ofstream file(path);
    file << "[device]\n"
         << "fingerprint_secret = \"f51-limiter-secret\"\n"
         << "\n"
         << "[rate_limit]\n"
         << "enabled = true\n"
         << "window_seconds = 60\n"
         << "max_requests = 2\n"
         << "lockout_threshold = 3\n"
         << "lockout_seconds = 300\n";
  }

  ConfigService::load(path);
  std::remove(path);

  RemoteGate gate(RemoteConfig{},
                  std::make_shared<RefreshRateLimiter>(
                      RateLimitConfig::resolve()));

  auto first = testRequest(drogon::Patch, "/auth/refresh-token");
  CHECK_FALSE(gate.check(first, false));
  CHECK_FALSE(
      gate.check(testRequest(drogon::Patch, "/auth/refresh-token"), false));

  auto third = testRequest(drogon::Patch, "/auth/refresh-token");
  const auto limitedResp = gate.check(third, false);
  const Json::Value body = parseBody(limitedResp);
  CHECK(body["status"].asInt() == 429);
  CHECK(body["errors"]["code"] == "TOO_MANY_REQUESTS");
  CHECK_FALSE(body["errors"]["message"].asString().empty());
  CHECK(body["info"].isNull());
  CHECK(limitedResp->getHeader("Access-Control-Allow-Origin") == "*");

  CHECK_FALSE(gate.check(testRequest(drogon::Post, "/pairing"), false));
  CHECK_FALSE(gate.check(testRequest(drogon::Get, "/health"), false));
  CHECK_FALSE(
      gate.check(testRequest(drogon::Get, "/auth/refresh-token"), false));
}

TEST_CASE("remote gate records outcomes into the lockout counter")
{
  RateLimitConfig config;
  config.enabled = true;
  config.maxRequests = 100;
  config.lockoutThreshold = 3;
  RemoteGate gate(
      RemoteConfig{}, std::make_shared<RefreshRateLimiter>(config));

  auto first = testRequest(drogon::Patch, "/auth/refresh-token");
  auto second = testRequest(drogon::Patch, "/auth/refresh-token");
  auto third = testRequest(drogon::Patch, "/auth/refresh-token");
  CHECK_FALSE(gate.check(first, false));
  CHECK_FALSE(gate.check(second, false));
  CHECK_FALSE(gate.check(third, false));

  gate.recordOutcome(first, AppConfig::get401Response());
  gate.recordOutcome(second, AppConfig::get401Response());

  auto fourth = testRequest(drogon::Patch, "/auth/refresh-token");
  CHECK_FALSE(gate.check(fourth, false));
  gate.recordOutcome(fourth, AppConfig::get401Response());

  const Json::Value locked = parseBody(
      gate.check(testRequest(drogon::Patch, "/auth/refresh-token"), false));
  CHECK(locked["status"].asInt() == 429);
  CHECK(locked["errors"]["code"] == "TOO_MANY_REQUESTS");

  RateLimitConfig reset;
  reset.enabled = true;
  reset.maxRequests = 100;
  reset.lockoutThreshold = 2;
  RemoteGate resetGate(
      RemoteConfig{}, std::make_shared<RefreshRateLimiter>(reset));

  auto failure = testRequest(drogon::Patch, "/auth/refresh-token");
  auto success = testRequest(drogon::Patch, "/auth/refresh-token");
  CHECK_FALSE(resetGate.check(failure, false));
  CHECK_FALSE(resetGate.check(success, false));
  resetGate.recordOutcome(failure, AppConfig::get401Response());
  resetGate.recordOutcome(success, ApiResponse::ok());
  CHECK_FALSE(resetGate.check(
      testRequest(drogon::Patch, "/auth/refresh-token"), false));
}
