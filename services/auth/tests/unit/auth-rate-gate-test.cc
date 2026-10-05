#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/auth-errors.hxx>
#include <config/auth-config.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/auth/infra/auth-rate-gate.hxx>
#include <http/api-response.hxx>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{

std::string configPath(const std::string& stem)
{
  return (std::filesystem::temp_directory_path()
          / (stem + "-" + std::to_string(::getpid()) + ".toml"))
      .string();
}

struct ConfigWriteInput
{
  std::string path;
  std::string body;
};

void writeConfig(const ConfigWriteInput& input)
{
  std::ofstream file(input.path);
  file << input.body;
}

[[nodiscard]] drogon::HttpRequestPtr
patchRequest(const std::string& userAgent,
             const std::string& address = "10.0.0.5")
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Patch);
  req->setPath("/auth/refresh-token");
  req->addHeader("User-Agent", userAgent);
  req->addHeader("X-Forwarded-For", address);
  return req;
}

[[nodiscard]] drogon::HttpRequestPtr
requestWith(drogon::HttpMethod method, const std::string& path)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method);
  req->setPath(path);
  req->addHeader("User-Agent", "argus-rate-gate/1.0");
  return req;
}

[[nodiscard]] drogon::HttpResponsePtr refusal()
{
  return ApiResponse::error(AuthErrors::AuthenticationRequired);
}

[[nodiscard]] AuthRateLimitConfig windowConfig()
{
  AuthRateLimitConfig config;
  config.enabled = true;
  config.windowSeconds = 60;
  config.maxRequests = 2;
  config.lockoutThreshold = 3;
  config.lockoutSeconds = 300;
  return config;
}

void loadGateConfig(const std::string& extra)
{
  const std::string path = configPath("argus-auth-rate-gate");
  writeConfig(
      {.path = path,
       .body = std::string("[jwt]\n"
                           "secret = \"argus-auth-rate-gate-secret-0123456789\"\n"
                           "\n"
                           "[device]\n"
                           "fingerprint_secret = \"argus-rate-gate-fingerprint\"\n"
                           "trust_forwarded_for = true\n"
                           "trusted_proxy_ips = \"" +
                           drogon::HttpRequest::newHttpRequest()
                               ->getPeerAddr()
                               .toIp() +
                           "\"\n"
                           "\n") + extra});
  ConfigService::load(path);
  std::filesystem::remove(path);
}

}

TEST_CASE("rate limit config resolves defaults and honors overrides")
{
  loadGateConfig("");
  const AuthRateLimitConfig config = AuthConfig::resolveRateLimit();

  CHECK(config.enabled);
  CHECK(config.windowSeconds == 60);
  CHECK(config.maxRequests == 10);
  CHECK(config.lockoutThreshold == 5);
  CHECK(config.lockoutSeconds == 300);

  loadGateConfig("[rate_limit]\n"
                 "enabled = true\n"
                 "window_seconds = 30\n"
                 "max_requests = 3\n"
                 "lockout_threshold = 2\n"
                 "lockout_seconds = 120\n");
  const AuthRateLimitConfig custom = AuthConfig::resolveRateLimit();

  CHECK(custom.enabled);
  CHECK(custom.windowSeconds == 30);
  CHECK(custom.maxRequests == 3);
  CHECK(custom.lockoutThreshold == 2);
  CHECK(custom.lockoutSeconds == 120);

  loadGateConfig("[rate_limit]\n"
                 "enabled = true\n"
                 "window_seconds = -5\n"
                 "max_requests = 0\n"
                 "lockout_threshold = 0\n"
                 "lockout_seconds = -1\n");
  const AuthRateLimitConfig guarded = AuthConfig::resolveRateLimit();

  CHECK(guarded.windowSeconds == 60);
  CHECK(guarded.maxRequests == 10);
  CHECK(guarded.lockoutThreshold == 5);
  CHECK(guarded.lockoutSeconds == 300);
}

TEST_CASE("the gate admits within the window and refuses past it")
{
  loadGateConfig("");

  AuthRateLimitConfig config = windowConfig();
  AuthRateGate gate(config);

  auto first = patchRequest("argus-app/1.0");
  CHECK_FALSE(gate.check(first));
  auto second = patchRequest("argus-app/1.0");
  CHECK_FALSE(gate.check(second));

  const auto limited = gate.check(patchRequest("argus-app/1.0"));
  REQUIRE(limited);
  CHECK(limited->getStatusCode() == drogon::k429TooManyRequests);
  CHECK(limited->getHeader("Access-Control-Allow-Origin").empty());
  const auto body = limited->getJsonObject();
  REQUIRE(body);
  CHECK((*body)["status"].asInt() == 429);
  CHECK((*body)["errors"]["code"].asString() == "TOO_MANY_REQUESTS");
  CHECK_FALSE((*body)["errors"]["message"].asString().empty());
  CHECK((*body)["info"].isNull());

  REQUIRE(gate.check(patchRequest("argus-tablet/1.0")));
  CHECK_FALSE(gate.check(patchRequest("argus-tablet/1.0", "10.0.0.6")));

  CHECK_FALSE(gate.check(requestWith(drogon::Post, "/auth/refresh-token")));
  CHECK_FALSE(gate.check(requestWith(drogon::Get, "/auth/refresh-token")));
  CHECK_FALSE(gate.check(requestWith(drogon::Patch, "/auth/me")));
  CHECK_FALSE(gate.check(requestWith(drogon::Get, "/health")));
  CHECK_FALSE(gate.check(requestWith(drogon::Post, "/auth/register")));

  auto variant = patchRequest("argus-app/1.0");
  variant->setPath("/auth/Refresh-Token");
  REQUIRE(gate.check(variant));
}

TEST_CASE("a disabled gate never refuses")
{
  AuthRateGate gate(AuthRateLimitConfig{});

  for (int i = 0; i < 10; ++i)
    CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));

  gate.recordOutcome(patchRequest("argus-app/1.0"), refusal());
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("the gate locks out a key after consecutive failures")
{
  loadGateConfig("");

  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  config.lockoutThreshold = 3;
  AuthRateGate gate(config);

  auto first = patchRequest("argus-app/1.0");
  auto second = patchRequest("argus-app/1.0");
  auto third = patchRequest("argus-app/1.0");
  CHECK_FALSE(gate.check(first));
  CHECK_FALSE(gate.check(second));
  CHECK_FALSE(gate.check(third));

  gate.recordOutcome(first, refusal());
  gate.recordOutcome(second, refusal());
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
  gate.recordOutcome(third, refusal());

  const auto locked = gate.check(patchRequest("argus-app/1.0"));
  REQUIRE(locked);
  CHECK(locked->getStatusCode() == drogon::k429TooManyRequests);
  CHECK_FALSE(gate.check(patchRequest("argus-tablet/1.0", "10.0.0.6")));

  AuthRateLimitConfig forgiving = windowConfig();
  forgiving.maxRequests = 100;
  forgiving.lockoutThreshold = 2;
  AuthRateGate resetGate(forgiving);

  auto failure = patchRequest("argus-app/1.0");
  auto success = patchRequest("argus-app/1.0");
  CHECK_FALSE(resetGate.check(failure));
  CHECK_FALSE(resetGate.check(success));
  resetGate.recordOutcome(failure, refusal());
  resetGate.recordOutcome(success, ApiResponse::ok());
  resetGate.recordOutcome(patchRequest("argus-app/1.0"), refusal());
  CHECK_FALSE(resetGate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("an expired window admits the key again")
{
  loadGateConfig("");

  AuthRateLimitConfig config = windowConfig();
  config.windowSeconds = 1;
  config.maxRequests = 1;
  AuthRateGate gate(config);

  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
  REQUIRE(gate.check(patchRequest("argus-app/1.0")));

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
  REQUIRE(gate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("an expired lockout admits the key again")
{
  loadGateConfig("");

  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  config.lockoutThreshold = 1;
  config.lockoutSeconds = 1;
  AuthRateGate gate(config);

  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
  gate.recordOutcome(patchRequest("argus-app/1.0"), refusal());
  REQUIRE(gate.check(patchRequest("argus-app/1.0")));

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  gate.recordOutcome(patchRequest("argus-app/1.0"), refusal());

  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("login, registration and device login are limited per address")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 1;
  AuthRateGate gate(config);

  const auto post = [](const std::string& path, const std::string& agent) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->addHeader("User-Agent", agent);
    req->addHeader("X-Forwarded-For", "10.0.0.9");
    return req;
  };
  for (const char* path : {"/auth/login", "/auth/register", "/auth/device-login"}) {
    CAPTURE(path);
    CHECK_FALSE(gate.check(post(path, "agent-one")));
    REQUIRE(gate.check(post(path, "agent-two")));
  }
}

TEST_CASE("rotating forwarded addresses from one peer meets the peer ceiling")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 1;
  AuthRateGate gate(config);

  const int ceiling = config.maxRequests * AuthRateGate::kPeerCeilingFactor;
  for (int attempt = 0; attempt < ceiling; ++attempt) {
    CAPTURE(attempt);
    CHECK_FALSE(gate.check(patchRequest("argus-app/1.0", "10.1.0." + std::to_string(attempt))));
  }
  REQUIRE(gate.check(patchRequest("argus-app/1.0", "10.2.0.1")));
}

TEST_CASE("a server failure is not counted against the caller")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  AuthRateGate gate(config);

  const auto unavailable = ApiResponse::error(AuthErrors::IdentityUnavailable);
  for (int attempt = 0; attempt < config.lockoutThreshold + 2; ++attempt) {
    CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
    gate.recordOutcome(patchRequest("argus-app/1.0"), unavailable);
  }
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("only 401 and 403 count as failures; a 422 or a 404 locks nobody out")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  AuthRateGate gate(config);

  const auto invalid = ApiResponse::error(AuthErrors::InvalidJsonBody);
  invalid->setStatusCode(drogon::k422UnprocessableEntity);
  const auto missing = ApiResponse::error(AuthErrors::ChallengeNotFound);
  for (int attempt = 0; attempt < config.lockoutThreshold + 2; ++attempt) {
    CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
    gate.recordOutcome(patchRequest("argus-app/1.0"), invalid);
    gate.recordOutcome(patchRequest("argus-app/1.0"), missing);
  }
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));

  const auto forbidden = ApiResponse::error(AuthErrors::AccountDisabled);
  for (int attempt = 0; attempt < config.lockoutThreshold; ++attempt)
    gate.recordOutcome(patchRequest("argus-app/1.0"), forbidden);
  CHECK(gate.check(patchRequest("argus-app/1.0")));
}

TEST_CASE("a failed liveness check counts like a face that did not match; poor quality does not")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  AuthRateGate gate(config);

  const auto blurred = ApiResponse::error(AuthErrors::FaceQualityInsufficient);
  const auto unavailable = ApiResponse::error(AuthErrors::LivenessUnavailable);
  for (int attempt = 0; attempt < config.lockoutThreshold + 2; ++attempt) {
    gate.recordOutcome(patchRequest("argus-app/1.0"), blurred);
    gate.recordOutcome(patchRequest("argus-app/1.0"), unavailable);
  }
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));

  const auto spoof = ApiResponse::error(AuthErrors::LivenessCheckFailed);
  CHECK(spoof->getStatusCode() == drogon::k401Unauthorized);
  for (int attempt = 0; attempt < config.lockoutThreshold; ++attempt)
    gate.recordOutcome(patchRequest("argus-app/1.0"), spoof);
  CHECK(gate.check(patchRequest("argus-app/1.0")));
}

[[nodiscard]] drogon::HttpRequestPtr refreshWith(const std::string& token)
{
  Json::Value body(Json::objectValue);
  body["refreshToken"] = token;
  auto req = drogon::HttpRequest::newHttpJsonRequest(body);
  req->setMethod(drogon::Patch);
  req->setPath("/auth/refresh-token");
  req->addHeader("User-Agent", "argus-app/1.0");
  req->addHeader("X-Forwarded-For", "10.0.0.5");
  return req;
}

TEST_CASE("a refresh with a verified session is limited by that session, not by the household address")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 100;
  AuthRateGate gate(config, [](const std::string& token) {
    return token.starts_with("valid-") ? token.substr(6) : std::string{};
  });

  for (int attempt = 0; attempt < config.lockoutThreshold; ++attempt) {
    CHECK_FALSE(gate.check(refreshWith("garbage")));
    gate.recordOutcome(refreshWith("garbage"), refusal());
  }
  CHECK(gate.check(refreshWith("garbage")));
  CHECK(gate.check(patchRequest("argus-app/1.0")));
  CHECK_FALSE(gate.check(refreshWith("valid-alice")));
  CHECK_FALSE(gate.check(refreshWith("valid-bob")));

  for (int attempt = 0; attempt < config.lockoutThreshold; ++attempt)
    gate.recordOutcome(refreshWith("valid-alice"), refusal());
  CHECK(gate.check(refreshWith("valid-alice")));
  CHECK_FALSE(gate.check(refreshWith("valid-bob")));
}

TEST_CASE("IPv6 clients are grouped per /64")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 2;
  AuthRateGate gate(config);

  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0", "2001:db8:1:2::5")));
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0", "2001:db8:1:2:ffff::9")));
  CHECK(gate.check(patchRequest("argus-app/1.0", "2001:db8:1:2:abcd::1")));
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0", "2001:db8:1:3::5")));
}

TEST_CASE("a full table never evicts a locked key")
{
  loadGateConfig("");
  AuthRateLimitConfig config = windowConfig();
  config.maxRequests = 1000;
  config.lockoutThreshold = 1;
  AuthRateGate gate(config);

  const auto locked = patchRequest("argus-app/1.0", "192.168.50.1");
  CHECK_FALSE(gate.check(locked));
  gate.recordOutcome(locked, refusal());
  REQUIRE(gate.check(patchRequest("argus-app/1.0", "192.168.50.1")));

  for (int index = 0; index < 5000; ++index) {
    const std::string address = "10." + std::to_string(index / 250) + "." +
                                std::to_string(index % 250) + ".1";
    static_cast<void>(gate.check(patchRequest("argus-app/1.0", address)));
  }
  CHECK(gate.check(patchRequest("argus-app/1.0", "192.168.50.1")));
}
