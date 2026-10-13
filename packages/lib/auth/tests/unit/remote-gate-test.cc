#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <auth/jwt-filter.hxx>
#include <auth/remote-config.hxx>
#include <auth/remote-gate.hxx>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <drogon/HttpRequest.h>
#include <http/api-response.hxx>
#include <http/listener-config.hxx>
#include <json/json.h>
#include <filesystem>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

struct KeyDeleter
{
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct X509Deleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

struct BioDeleter
{
  void operator()(BIO* bio) const { BIO_free(bio); }
};

using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;

struct CertificateFile
{
  std::string path;
  long lifetimeSeconds{0};
};

Key newKey()
{
  return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
}

std::string tempPath(const std::string& stem)
{
  return (std::filesystem::temp_directory_path() /
          (stem + "-" + std::to_string(::getpid()) + ".pem"))
      .string();
}

void writeKey(const std::string& path, EVP_PKEY* key)
{
  const std::unique_ptr<BIO, BioDeleter> file(BIO_new_file(path.c_str(), "wb"));
  REQUIRE(file);
  REQUIRE(PEM_write_bio_PrivateKey(file.get(), key, nullptr, nullptr, 0, nullptr,
                                   nullptr) == 1);
}

void writeCertificate(const CertificateFile& file, EVP_PKEY* key)
{
  const std::unique_ptr<X509, X509Deleter> certificate(X509_new());
  REQUIRE(certificate);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -7200);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), file.lifetimeSeconds);
  X509_set_pubkey(certificate.get(), key);
  const std::string commonName = "sync--0123456789abcdef0123456789abcdef.argus-tunnel.lynk.page";
  const std::vector<unsigned char> nameBytes(commonName.begin(), commonName.end());
  X509_NAME* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, nameBytes.data(),
                             static_cast<int>(nameBytes.size()), -1, 0);
  X509_set_issuer_name(certificate.get(), name);
  REQUIRE(X509_sign(certificate.get(), key, EVP_sha256()) > 0);
  const std::unique_ptr<BIO, BioDeleter> out(BIO_new_file(file.path.c_str(), "wb"));
  REQUIRE(out);
  REQUIRE(PEM_write_bio_X509(out.get(), certificate.get()) == 1);
}

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

TEST_CASE("remote config reads the tunnel certificate pair")
{
  CHECK(RemoteConfig::resolve().tunnelCert.empty());
  CHECK(RemoteConfig::resolve().tunnelKey.empty());
  ConfigService::setRuntimeString("remote.tunnel_cert", "certs/tunnel/sync.pem");
  ConfigService::setRuntimeString("remote.tunnel_key", "certs/tunnel/sync.key");
  const RemoteConfig paired = RemoteConfig::resolve();
  CHECK(paired.tunnelCert == "certs/tunnel/sync.pem");
  CHECK(paired.tunnelKey == "certs/tunnel/sync.key");
  ConfigService::setRuntimeString("remote.tunnel_cert", "");
  ConfigService::setRuntimeString("remote.tunnel_key", "");
}

TEST_CASE("the tunnel listener presents its own pair only while the pair is usable")
{
  const std::string certPath = tempPath("remote-gate-tunnel-cert");
  const std::string keyPath = tempPath("remote-gate-tunnel-key");
  const std::string otherKeyPath = tempPath("remote-gate-other-key");
  const std::string expiredPath = tempPath("remote-gate-expired-cert");
  const Key key = newKey();
  const Key otherKey = newKey();
  REQUIRE(key);
  REQUIRE(otherKey);
  writeKey(keyPath, key.get());
  writeKey(otherKeyPath, otherKey.get());
  writeCertificate({.path = certPath, .lifetimeSeconds = 3600}, key.get());
  writeCertificate({.path = expiredPath, .lifetimeSeconds = -3600}, key.get());

  const ListenerConfig base = tlsListener();
  const auto tunnelListener = [&base](const RemoteConfig& remote) {
    Json::Value listeners = listenerJson(base);
    appendRemoteListener({.listeners = listeners, .remote = remote, .base = base});
    REQUIRE(listeners.size() == 2);
    CHECK(listeners[0]["cert"] == base.certPath);
    CHECK(listeners[0]["key"] == base.keyPath);
    return listeners[1];
  };

  RemoteConfig paired;
  paired.tunnelPort = 17443;
  paired.tunnelCert = certPath;
  paired.tunnelKey = keyPath;
  const Json::Value own = tunnelListener(paired);
  CHECK(own["port"].asInt() == 17443);
  CHECK(own["cert"] == certPath);
  CHECK(own["key"] == keyPath);
  CHECK(own["address"] == base.host);
  CHECK(own["ssl_conf"][0][1] == base.minTlsProtocol);
  const ListenerConfig watched = remoteListenerConfig(paired, base);
  CHECK(watched.port == 17443);
  CHECK(watched.certPath == certPath);
  CHECK(watched.keyPath == keyPath);

  RemoteConfig mismatched = paired;
  mismatched.tunnelKey = otherKeyPath;
  CHECK(tunnelListener(mismatched)["cert"] == base.certPath);

  RemoteConfig expired = paired;
  expired.tunnelCert = expiredPath;
  CHECK(tunnelListener(expired)["cert"] == base.certPath);

  RemoteConfig halfPaired = paired;
  halfPaired.tunnelKey.clear();
  const Json::Value half = tunnelListener(halfPaired);
  CHECK(half["cert"] == base.certPath);
  CHECK(half["key"] == base.keyPath);

  RemoteConfig missing = paired;
  missing.tunnelCert = "/nonexistent/tunnel.pem";
  CHECK(tunnelListener(missing)["cert"] == base.certPath);

  const ListenerConfig plain = plainListener();
  Json::Value plainListeners = listenerJson(plain);
  appendRemoteListener({.listeners = plainListeners, .remote = paired, .base = plain});
  REQUIRE(plainListeners.size() == 2);
  CHECK_FALSE(plainListeners[1].isMember("cert"));

  for (const std::string& path : {certPath, keyPath, otherKeyPath, expiredPath})
    std::filesystem::remove(path);
}

TEST_CASE("a sealed gate refuses every tunnelled request that carries no session")
{
  RemoteConfig config;
  config.tunnelPort = 17443;
  RemoteGate sealed(config, RemoteSurface::Sealed);

  const auto refusal = sealed.check(testRequest(drogon::Get, "/health"), true);
  REQUIRE(refusal);
  const Json::Value body = parseBody(refusal);
  CHECK(body["status"].asInt() == 403);
  CHECK(body["errors"]["code"] == "REMOTE_NOT_ALLOWED");

  auto bearer = testRequest(drogon::Get, "/camera");
  bearer->addHeader("Authorization", "Bearer an-access-token");
  CHECK_FALSE(sealed.check(bearer, true));

  auto basic = testRequest(drogon::Get, "/camera");
  basic->addHeader("Authorization", "Basic dXNlcjpwYXNz");
  CHECK(sealed.check(basic, true));

  auto upgrade = testRequest(drogon::Get, "/sync");
  upgrade->addHeader("Upgrade", "websocket");
  upgrade->setParameter("token", "an-access-token");
  CHECK_FALSE(sealed.check(upgrade, true));

  auto queryOnly = testRequest(drogon::Get, "/camera");
  queryOnly->setParameter("token", "an-access-token");
  CHECK(sealed.check(queryOnly, true));

  CHECK(sealed.check(testRequest(drogon::Options, "/camera"), true));
  CHECK_FALSE(sealed.check(testRequest(drogon::Get, "/health"), false));
  CHECK_FALSE(sealed.checkRequest(testRequest(drogon::Get, "/health")));

  config.enabled = true;
  config.allowQrLogin = true;
  RemoteGate stillSealed(config, RemoteSurface::Sealed);
  CHECK(stillSealed.check(testRequest(drogon::Post, "/pairing"), true));
  CHECK(stillSealed.check(testRequest(drogon::Get, "/health"), true));

  RemoteGate publicGate(config, RemoteSurface::Public);
  CHECK_FALSE(publicGate.check(testRequest(drogon::Get, "/health"), true));
}

TEST_CASE("a sealed gate reads the session where JwtFilter reads it, and leaves the LAN alone")
{
  RemoteConfig config;
  config.tunnelPort = 17443;
  const RemoteGate sealed(config, RemoteSurface::Sealed);

  auto header = testRequest(drogon::Get, "/camera");
  header->addHeader("Authorization", "Bearer header-token");
  CHECK(JwtFilter::extractToken(header) == "header-token");
  CHECK_FALSE(sealed.check(header, true));

  auto socket = testRequest(drogon::Get, "/sync");
  socket->addHeader("Upgrade", "websocket");
  socket->setParameter("token", "query-token");
  CHECK(JwtFilter::extractToken(socket) == "query-token");
  CHECK_FALSE(sealed.check(socket, true));

  auto cookie = testRequest(drogon::Get, "/camera");
  cookie->addCookie("token", "cookie-token");
  cookie->addCookie("access_token", "cookie-token");
  CHECK(JwtFilter::extractToken(cookie).empty());
  const auto cookieRefusal = sealed.check(cookie, true);
  REQUIRE(cookieRefusal);
  CHECK(parseBody(cookieRefusal)["errors"]["code"] == "REMOTE_NOT_ALLOWED");

  auto cookieSocket = testRequest(drogon::Get, "/sync");
  cookieSocket->addHeader("Upgrade", "websocket");
  cookieSocket->addCookie("token", "cookie-token");
  CHECK(sealed.check(cookieSocket, true));

  const auto bare = testRequest(drogon::Get, "/camera");
  for (const auto& request : {header, socket, cookie, cookieSocket, bare}) {
    CHECK_FALSE(sealed.check(request, false));
    CHECK_FALSE(sealed.checkRequest(request));
  }
}
