#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/auth-errors.hxx>
#include <config/auth-config.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/auth/infra/refresh-rate-gate.hxx>
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
patchRequest(const std::string& userAgent)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Patch);
  req->setPath("/auth/refresh-token");
  req->addHeader("User-Agent", userAgent);
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
                           "\n") + extra});
  ConfigService::load(path);
  std::filesystem::remove(path);
}

}

TEST_CASE("rate limit config resolves defaults and honors overrides")
{
  loadGateConfig("");
  const AuthRateLimitConfig config = AuthConfig::resolveRateLimit();

  CHECK_FALSE(config.enabled);
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
  RefreshRateGate gate(config);

  auto first = patchRequest("argus-app/1.0");
  CHECK_FALSE(gate.check(first));
  auto second = patchRequest("argus-app/1.0");
  CHECK_FALSE(gate.check(second));

  const auto limited = gate.check(patchRequest("argus-app/1.0"));
  REQUIRE(limited);
  CHECK(limited->getStatusCode() == drogon::k429TooManyRequests);
  CHECK(limited->getHeader("Access-Control-Allow-Origin") == "*");
  const auto body = limited->getJsonObject();
  REQUIRE(body);
  CHECK((*body)["status"].asInt() == 429);
  CHECK((*body)["errors"]["code"].asString() == "TOO_MANY_REQUESTS");
  CHECK_FALSE((*body)["errors"]["message"].asString().empty());
  CHECK((*body)["info"].isNull());

  CHECK_FALSE(gate.check(patchRequest("argus-tablet/1.0")));

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
  RefreshRateGate gate(AuthRateLimitConfig{});

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
  RefreshRateGate gate(config);

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
  CHECK_FALSE(gate.check(patchRequest("argus-tablet/1.0")));

  AuthRateLimitConfig forgiving = windowConfig();
  forgiving.maxRequests = 100;
  forgiving.lockoutThreshold = 2;
  RefreshRateGate resetGate(forgiving);

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
  RefreshRateGate gate(config);

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
  RefreshRateGate gate(config);

  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
  gate.recordOutcome(patchRequest("argus-app/1.0"), refusal());
  REQUIRE(gate.check(patchRequest("argus-app/1.0")));

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  gate.recordOutcome(patchRequest("argus-app/1.0"), refusal());

  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK_FALSE(gate.check(patchRequest("argus-app/1.0")));
}
