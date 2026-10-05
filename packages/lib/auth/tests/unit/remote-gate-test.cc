#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <auth/remote-config.hxx>
#include <auth/remote-gate.hxx>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <drogon/HttpRequest.h>
#include <http/api-response.hxx>
#include <http/listener-config.hxx>
#include <json/json.h>
#include <stdexcept>
#include <string>

namespace
{

drogon::HttpRequestPtr testRequest(drogon::HttpMethod method,
                                   const std::string& path)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method);
  req->setPath(path);
  return req;
}

Json::Value parseBody(const drogon::HttpResponsePtr& response)
{
  Json::Value body;
  Json::Reader reader;
  const std::string text(response->getBody().begin(),
                         response->getBody().end());
  CHECK(reader.parse(text, body));
  return body;
}

ListenerConfig tlsListener()
{
  return ListenerConfig{.host = "0.0.0.0",
                        .port = 7042,
                        .tls = true,
                        .certPath = "certs/server.pem",
                        .keyPath = "certs/server.key",
                        .minTlsProtocol = "TLSv1.2"};
}

ListenerConfig plainListener()
{
  return ListenerConfig{.host = "0.0.0.0",
                        .port = 7052,
                        .tls = false,
                        .certPath = {},
                        .keyPath = {},
                        .minTlsProtocol = {}};
}

}

TEST_CASE("remote config resolves disabled by default and honors overrides")
{
  const RemoteConfig unset = RemoteConfig::resolve();
  CHECK(unset.tunnelPort == 0);
  CHECK_FALSE(unset.enabled);

  ConfigService::setRuntimeString("remote.tunnel_port", "17443");
  ConfigService::setRuntimeString("remote.enabled", "true");

  const RemoteConfig enabled = RemoteConfig::resolve();
  CHECK(enabled.tunnelPort == 17443);
  CHECK(enabled.enabled);

  ConfigService::setRuntimeString("remote.tunnel_port", "70000");
  CHECK(RemoteConfig::resolve().tunnelPort == 0);

  ConfigService::setRuntimeString("remote.tunnel_port", "0");
  ConfigService::setRuntimeString("remote.enabled", "false");

  const RemoteConfig zeroed = RemoteConfig::resolve();
  CHECK(zeroed.tunnelPort == 0);
  CHECK_FALSE(zeroed.enabled);
}

TEST_CASE("remote listener appends the tunnel listener only when configured")
{
  const ListenerConfig base = tlsListener();
  const RemoteConfig disabled;

  Json::Value listeners = listenerJson(base);
  appendRemoteListener(
      {.listeners = listeners, .remote = disabled, .base = base});
  CHECK(listeners.size() == 1);

  RemoteConfig enabled;
  enabled.tunnelPort = 17443;
  listeners = listenerJson(base);
  appendRemoteListener(
      {.listeners = listeners, .remote = enabled, .base = base});
  REQUIRE(listeners.size() == 2);
  CHECK(listeners[1]["address"] == base.host);
  CHECK(listeners[1]["port"].asInt() == 17443);
  CHECK(listeners[1]["https"].asBool() == base.tls);
  CHECK(listeners[1]["cert"] == base.certPath);
  CHECK(listeners[1]["key"] == base.keyPath);
  CHECK(listeners[1]["ssl_conf"][0][0] == "MinProtocol");
  CHECK(listeners[1]["ssl_conf"][0][1] == base.minTlsProtocol);
}

TEST_CASE("remote listener mirrors a plain service listener")
{
  const ListenerConfig base = plainListener();
  RemoteConfig enabled;
  enabled.tunnelPort = 17443;

  Json::Value listeners = listenerJson(base);
  appendRemoteListener(
      {.listeners = listeners, .remote = enabled, .base = base});
  REQUIRE(listeners.size() == 2);
  CHECK(listeners[1]["https"].asBool() == false);
  CHECK_FALSE(listeners[1].isMember("cert"));
  CHECK_FALSE(listeners[1].isMember("ssl_conf"));
}

TEST_CASE("tunnel port validation refuses a service listener collision")
{
  const ListenerConfig base = tlsListener();

  CHECK_NOTHROW(requireDistinctTunnelPort(base, RemoteConfig{}));

  RemoteConfig tunnel;
  tunnel.tunnelPort = 17443;
  CHECK_NOTHROW(requireDistinctTunnelPort(base, tunnel));

  RemoteConfig collision;
  collision.tunnelPort = base.port;
  CHECK_THROWS_AS(requireDistinctTunnelPort(base, collision),
                  std::runtime_error);
}

TEST_CASE("a tunnel without its own listener refuses to start")
{
  CHECK_NOTHROW(requireTunnelListener(RemoteConfig{}));

  RemoteConfig profileOnly;
  profileOnly.tunnelProfile = true;
  CHECK_THROWS_AS(requireTunnelListener(profileOnly), std::runtime_error);

  RemoteConfig enabledOnly;
  enabledOnly.enabled = true;
  CHECK_THROWS_AS(requireTunnelListener(enabledOnly), std::runtime_error);

  RemoteConfig qrOnly;
  qrOnly.allowQrLogin = true;
  CHECK_THROWS_AS(requireTunnelListener(qrOnly), std::runtime_error);

  RemoteConfig listening;
  listening.tunnelProfile = true;
  listening.allowQrLogin = true;
  listening.tunnelPort = 17443;
  CHECK_NOTHROW(requireTunnelListener(listening));
}

TEST_CASE("remote config reads the tunnel profile and the QR opt-in")
{
  ConfigService::setRuntimeString("remote.tunnel_profile", "true");
  ConfigService::setRuntimeString("remote.allow_qr_login", "true");
  const RemoteConfig armed = RemoteConfig::resolve();
  CHECK(armed.tunnelProfile);
  CHECK(armed.allowQrLogin);
  ConfigService::setRuntimeString("remote.tunnel_profile", "false");
  ConfigService::setRuntimeString("remote.allow_qr_login", "false");
  const RemoteConfig cleared = RemoteConfig::resolve();
  CHECK_FALSE(cleared.tunnelProfile);
  CHECK_FALSE(cleared.allowQrLogin);
}

TEST_CASE("a request without a tunnel port is never remote")
{
  CHECK_FALSE(
      requestIsRemote(testRequest(drogon::Get, "/health"), RemoteConfig{}));

  RemoteConfig enabled;
  enabled.tunnelPort = 17443;
  CHECK_FALSE(
      requestIsRemote(testRequest(drogon::Get, "/health"), enabled));
}

TEST_CASE("remote gate rejects pairing and register for remote requests")
{
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
  CHECK(pairingResp->getHeader("Access-Control-Allow-Origin").empty());

  auto registration = testRequest(drogon::Post, "/auth/register");
  const Json::Value registerBody = parseBody(gate.check(registration, true));
  CHECK(registerBody["status"].asInt() == 403);
  CHECK(registerBody["errors"]["code"] == "REMOTE_NOT_ALLOWED");

  for (const char* variant : {"/PAIRING", "/Pairing", "/Auth/Register", "/AUTH/REGISTER"}) {
    const auto refused = gate.check(testRequest(drogon::Post, variant), true);
    REQUIRE(refused);
    CHECK(parseBody(refused)["errors"]["code"] == "REMOTE_NOT_ALLOWED");
  }

  CHECK_FALSE(gate.check(testRequest(drogon::Post, "/pairing"), false));
  CHECK_FALSE(gate.check(testRequest(drogon::Get, "/health"), true));

  config.enabled = true;
  RemoteGate openGate(config);
  CHECK_FALSE(openGate.check(testRequest(drogon::Post, "/pairing"), true));
  CHECK_FALSE(
      openGate.check(testRequest(drogon::Post, "/auth/register"), true));
}
