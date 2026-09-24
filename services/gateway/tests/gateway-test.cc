#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <proxy/proxy-config.hxx>
#include <server/remote-config.hxx>
#include <server/remote-gate.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <http/api-response.hxx>
#include <proxy/reverse-proxy.hxx>
#include <sync/camera-stream-relay.hxx>
#include <sync/camera-stream-socket.hxx>

#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <trantor/net/EventLoop.h>
#include <vector>

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

drogon::HttpRequestPtr testRequest(drogon::HttpMethod method,
                                   const std::string& path)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method);
  req->setPath(path);
  req->addHeader("User-Agent", "gateway-test");
  return req;
}

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "exec failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
  sqlite3_free(error);
}

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    const std::string message = raw ? sqlite3_errmsg(raw) : "open failed";
    sqlite3_close_v2(raw);
    throw std::runtime_error(message);
  }
  return {raw, sqlite3_close_v2};
}

void drain(const drogon::orm::DbClientPtr& client)
{
  auto drained = std::make_shared<std::promise<void>>();
  auto done = drained->get_future();
  client->execSqlAsync(
      "SELECT 1",
      [drained](const drogon::orm::Result&) {
        trantor::EventLoop::getEventLoopOfCurrentThread()->queueInLoop(
            [drained]() { drained->set_value(); });
      },
      [drained](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        }
        catch (const std::exception& ex) {
          std::fprintf(stderr, "drain statement failed: %s\n", ex.what());
        }
        drained->set_value();
      });
  if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    throw std::runtime_error("the client's loop did not drain");
}

}

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

TEST_CASE("runtime override wins over the config file value")
{
  const char* path = "gateway-test-config-runtime.toml";
  {
    std::ofstream file(path);
    file << "[test]\n"
         << "value = \"file\"\n";
  }

  ConfigService::load(path);
  CHECK(ConfigService::getString("test.value") == "file");

  ConfigService::setRuntimeString("test.value", "runtime");
  CHECK(ConfigService::getString("test.value") == "runtime");

  std::remove(path);
}

TEST_CASE("gateway config section resolves listener and remote keys")
{
  const char* path = "gateway-test-config.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n"
         << "host = \"127.0.0.1\"\n"
         << "\n"
         << "[remote]\n"
         << "tunnel_port = 0\n";
  }

  ConfigService::load(path);

  CHECK(ConfigService::getInt("gateway.port") == 7024);
  CHECK(ConfigService::getString("gateway.host") == "127.0.0.1");
  CHECK(ConfigService::getInt("remote.tunnel_port") == 0);

  std::remove(path);
}

TEST_CASE("camera stream frames are the only ones on the media socket")
{
  struct Row
  {
    const char* type;
    bool allowed;
  };
  static const Row table[] = {
      {"camera:subscribe", true},
      {"camera:ready", true},
      {"camera:closed", true},
      {"camera:ack", true},
      {"camera:unsubscribe", true},
      {"camera:subscribe_error", true},
      {"voice:start", false},
      {"sync", false},
      {"camera", false},
      {"cameraX", false},
      {"", false},
  };

  for (const auto& row : table)
    CHECK(isCameraStreamFrame(row.type) == row.allowed);
}

TEST_CASE("read-only legacy database rejects writes and serves reads")
{
  const char* dbPath = "gateway-test-readonly.db";
  std::remove(dbPath);

  DbService::enableUriFilenames();
  {
    const DbHandle db = openFile(dbPath);
    exec(db.get(),
         "CREATE TABLE note (id INTEGER PRIMARY KEY, text TEXT NOT NULL)");
    exec(db.get(), "INSERT INTO note (text) VALUES ('hello')");
  }

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

  drain(readOnly);
  DbService::setReadOnlyClient(nullptr);
  std::remove(dbPath);
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

  REQUIRE(config.exclusions.size() == 2);
  const std::vector<std::string> expected = {
      "/camera-stream",
      "/health",
  };
  CHECK(config.exclusions == expected);
  CHECK(config.authProxyUrl.empty());
  CHECK(config.identityProxyUrl.empty());
  CHECK(config.cameraProxyUrl.empty());
  CHECK(config.productivityProxyUrl.empty());
  CHECK(config.notificationProxyUrl.empty());

  std::remove(path);
}

TEST_CASE("proxy config routes the identity domain to argus-identity")
{
  const char* path = "gateway-test-config-identity-proxy.toml";
  {
    std::ofstream file(path);
    file << "[identity]\n"
         << "proxy_url = \"https://127.0.0.1:7044\"\n";
  }

  ConfigService::load(path);
  const ProxyConfig config = ProxyConfig::resolve();

  CHECK(config.identityProxyUrl == "https://127.0.0.1:7044");

  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const ProxyConfig unrouted = ProxyConfig::resolve();
  CHECK(unrouted.identityProxyUrl.empty());

  std::remove(path);
}

TEST_CASE("route table sends the whole identity domain to the identity backend")
{
  gateway_proxy::SimpleReverseProxy proxy;
  Json::Value config;
  Json::Value routes(Json::arrayValue);
  Json::Value identityRoute(Json::objectValue);
  Json::Value prefixes(Json::arrayValue);
  prefixes.append("/invitation");
  prefixes.append("/pairing");
  prefixes.append("/portrait-preview");
  prefixes.append("/user");
  identityRoute["prefixes"] = prefixes;
  identityRoute["max_segments"] = 4;
  identityRoute["backend"] = "https://127.0.0.1:7044";
  identityRoute["validate_cert"] = false;
  routes.append(identityRoute);
  config["routes"] = routes;

  proxy.initAndStart(config);

  CHECK(proxy.matchRoute("/invitation") == 0);
  CHECK(proxy.matchRoute("/invitation/resolve") == 0);
  CHECK(proxy.matchRoute("/invitation/7") == 0);
  CHECK(proxy.matchRoute("/pairing") == 0);
  CHECK(proxy.matchRoute("/user") == 0);
  CHECK(proxy.matchRoute("/user/3") == 0);
  CHECK(proxy.matchRoute("/portrait-preview/3") == 0);
  CHECK(proxy.matchRoute("/portrait-preview/token-a/content") == 0);
  CHECK(proxy.matchRoute("/portrait-preview/token-a/content/x") == 0);
  CHECK(proxy.matchRoute("/portrait-preview/a/b/c/d/e") == -1);
  CHECK(proxy.matchRoute("/users/3") == -1);
  CHECK(proxy.matchRoute("/pairings") == -1);
  CHECK(proxy.matchRoute("/camera/1") == -1);

  proxy.shutdown();
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

TEST_CASE("camera stream config resolves the media target")
{
  const char* path = "gateway-test-config-camera-stream.toml";
  {
    std::ofstream file(path);
    file << "[camera]\n"
         << "stream_url = \"ws://127.0.0.1:7026/media\"\n";
  }

  ConfigService::load(path);
  const CameraStreamConfig config = CameraStreamConfig::resolve();
  CHECK(config.streamUrl == "ws://127.0.0.1:7026/media");

  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const CameraStreamConfig fallback = CameraStreamConfig::resolve();
  CHECK(fallback.streamUrl.empty());

  std::remove(path);
}

TEST_CASE("camera stream relay rejects every non-camera frame")
{
  CameraStreamRelay relay("ws://127.0.0.1:7026/media");
  const drogon::WebSocketConnectionPtr conn;

  for (const char* type : {"voice:start", "sync", "camera", "cameraX", ""}) {
    Json::Value frame;
    frame["type"] = type;
    CHECK_FALSE(drogon::sync_wait(
        relay.forwardText({.conn = conn, .message = frame, .raw = ""})));
  }
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

  proxy.initAndStart(config);

  CHECK(proxy.matchRoute("/camera") == 0);
  CHECK(proxy.matchRoute("/camera/1") == 0);
  CHECK(proxy.matchRoute("/zone") == 0);
  CHECK(proxy.matchRoute("/zone/3") == 0);

  CHECK(proxy.matchRoute("/camera/1/ptz") == 0);
  CHECK(proxy.matchRoute("/camera/1/preset") == 0);
  CHECK(proxy.matchRoute("/camera/1/settings") == 0);
  CHECK(proxy.matchRoute("/camera/1/status") == 0);
  CHECK(proxy.matchRoute("/camera/1/presets") == 0);
  CHECK(proxy.matchRoute("/camera/1/capabilities") == 0);
  CHECK(proxy.matchRoute("/camera/1/talk") == 0);
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
  const std::vector<std::string> exclusions = {"/camera-stream", "/health"};

  CHECK(isGatewayNativePath("/health", exclusions));
  CHECK(isGatewayNativePath("/camera-stream", exclusions));
  CHECK_FALSE(isGatewayNativePath("/camera-streamx", exclusions));
  CHECK_FALSE(isGatewayNativePath("/healthx", exclusions));
  CHECK_FALSE(isGatewayNativePath("/camera", exclusions));
  CHECK_FALSE(isGatewayNativePath("/camera/1/status", exclusions));
  CHECK_FALSE(isGatewayNativePath("/user", exclusions));
  CHECK_FALSE(isGatewayNativePath("/user/1", exclusions));
}

TEST_CASE("proxy exclusion set covers every registered gateway route")
{
  const char* path = "gateway-test-config-coverage.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  std::remove(path);

  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-gateway", .extras = {}}));
  drogon::app().registerController(std::make_shared<CameraStreamSocket>());

  const ProxyConfig proxy = ProxyConfig::resolve();
  REQUIRE_FALSE(proxy.exclusions.empty());

  int routes = 0;
  for (const auto& handlerInfo : drogon::app().getHandlersInfo()) {
    const auto& pattern = std::get<0>(handlerInfo);
    if (pattern.empty() || pattern.front() != '/')
      continue;
    ++routes;
    CHECK_MESSAGE(isGatewayNativePath(pattern, proxy.exclusions),
                  pattern);
  }
  CHECK(routes >= 1);
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
  appendRemoteListener({.listeners = listeners, .remote = disabled, .base = base});
  CHECK(listeners.size() == 1);

  RemoteConfig enabled;
  enabled.tunnelPort = 17443;
  listeners = listenerJson(base);
  appendRemoteListener({.listeners = listeners, .remote = enabled, .base = base});
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
  appendRemoteListener({.listeners = plainListeners, .remote = enabled, .base = plainBase});
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
  RemoteGate gate(config);

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
  RemoteGate openGate(config);
  CHECK_FALSE(openGate.check(testRequest(drogon::Post, "/pairing"), true));
  CHECK_FALSE(openGate.check(testRequest(drogon::Post, "/auth/register"),
                             true));
}

TEST_CASE("remote gate marks tunnel requests with the remote attribute")
{
  RemoteConfig config;
  config.tunnelPort = 17443;
  RemoteGate gate(config);

  auto sync = testRequest(drogon::Get, "/sync");
  CHECK_FALSE(gate.check(sync, true));
  CHECK(sync->getAttributes()->find(RemoteGate::kRemoteContextKey));
  CHECK(sync->getAttributes()->get<bool>(RemoteGate::kRemoteContextKey));

  auto local = testRequest(drogon::Get, "/sync");
  CHECK_FALSE(gate.check(local, false));
  CHECK_FALSE(local->getAttributes()->find(RemoteGate::kRemoteContextKey));
}
