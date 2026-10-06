#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fake-llm-server.hxx"

#include <chrono>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <exception>
#include <llm/llm-client.hxx>
#include <llm/llm-errors.hxx>
#include <llm/llm-remote.hxx>
#include <llm/llm-service.hxx>
#include <stop_token>
#include <string>
#include <vector>

namespace
{

constexpr const char* kDefaultTimeoutMs = "120000";
constexpr const char* kSecret = "rpc-secret";

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("llm.remote_url", url);
  ConfigService::setRuntimeString("llm.remote_timeout_ms", kDefaultTimeoutMs);
}

void pointRpcAt(const std::string& target, const std::string& credential)
{
  ConfigService::setRuntimeString("llm.grpc_target", target);
  ConfigService::setRuntimeString("llm.grpc_credential", credential);
}

struct Refusal
{
  int status{0};
  std::string code;
  std::string message;

  bool operator==(const Refusal&) const = default;
};

template <typename Call>
Refusal refusalBy(Call call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(),
            .code = error.errorCode(),
            .message = error.what()};
  }
  return {};
}

template <typename Call>
std::string thrownBy(Call call)
{
  try {
    call();
  }
  catch (const std::exception& error) {
    return error.what();
  }
  return "";
}

ChatRequest greeting()
{
  ChatRequest request;
  request.messages = {{.role = "user", .content = "Di hola"}};
  request.maxTokens = 16;
  return request;
}

std::vector<std::string> greetingTokens()
{
  return {"Hola", " de", " nuevo", ".", " Otra", " frase", "."};
}

void streamOnce(const LlmHttpClient& client)
{
  LlmStreamInput input;
  input.request = greeting();
  input.onToken = [](const std::string&, bool) {};
  client.chatStream(input);
}

argus::llm::ClientConfig clientConfig(std::string target,
                                      std::string credential,
                                      std::chrono::milliseconds timeout)
{
  return {.target = std::move(target),
          .credential = std::move(credential),
          .timeout = timeout};
}

}

TEST_CASE("The llm http client chats and streams over the argus-llm wire")
{
  const std::vector<std::string> tokens = greetingTokens();
  FakeLlmServer server({.tokens = tokens});
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  const LlmRemoteConfig config = LlmRemoteConfig::resolve();
  REQUIRE(config.enabled());
  const LlmHttpClient client(config.url, config.timeoutMs);

  CHECK(client.chat(greeting()) == "Hola de nuevo. Otra frase.");
  CHECK(server.requests().at("POST /llm/v1/chat") == 1);

  std::vector<std::string> arrived;
  LlmPrefillStats stats;
  LlmStreamInput input;
  input.request = greeting();
  input.stats = &stats;
  bool done = false;
  input.onToken = [&](const std::string& token, bool atEnd) {
    if (atEnd)
      done = true;
    else
      arrived.push_back(token);
  };
  client.chatStream(input);

  CHECK(done);
  CHECK(arrived == tokens);
  CHECK(stats.promptTokens == 7);
  CHECK(stats.reusedTokens == 0);
  CHECK(stats.decodedTokens == 7);
  CHECK(server.requests().at("POST /llm/v1/chat-stream") == 1);

  FakeLlmServer coalesced({.tokens = tokens, .coalesce = true});
  const std::string coalescedUrl =
      "http://127.0.0.1:" + std::to_string(coalesced.port());
  const LlmHttpClient oneChunk(coalescedUrl, config.timeoutMs);
  std::vector<std::string> coalescedTokens;
  LlmStreamInput coalescedInput;
  coalescedInput.request = greeting();
  coalescedInput.onToken = [&](const std::string& token, bool atEnd) {
    if (!atEnd)
      coalescedTokens.push_back(token);
  };
  oneChunk.chatStream(coalescedInput);
  REQUIRE(coalescedTokens.size() == 1);
  CHECK(coalescedTokens.front() == "Hola de nuevo. Otra frase.");
  CHECK(coalesced.requests().at("POST /llm/v1/chat-stream") == 1);

  const LlmHttpClient bare("127.0.0.1:" + std::to_string(server.port()),
                           config.timeoutMs);
  CHECK(bare.chat(greeting()) == "Hola de nuevo. Otra frase.");
  CHECK(server.requests().at("POST /llm/v1/chat") == 2);

  pointAt("");
}

TEST_CASE("The llm http client maps a refusal and a truncated stream into errors")
{
  FakeLlmServer down({.tokens = greetingTokens(), .status = 503});
  const std::string downUrl = "http://127.0.0.1:" + std::to_string(down.port());
  const LlmHttpClient refused(downUrl, 1000);

  CHECK(thrownBy([&] { (void)refused.chat(greeting()); }) ==
        "argus-llm LLM_NOT_LOADED: engine down");
  CHECK(thrownBy([&] { streamOnce(refused); }) ==
        "argus-llm LLM_NOT_LOADED: engine down");

  FakeLlmServer cut({.tokens = greetingTokens(), .truncated = true});
  const std::string cutUrl = "http://127.0.0.1:" + std::to_string(cut.port());
  const LlmHttpClient cutClient(cutUrl, 1000);
  CHECK(thrownBy([&] { streamOnce(cutClient); }) ==
        "argus-llm chunked body truncated");
  CHECK(cut.requests().at("POST /llm/v1/chat-stream") == 1);
}

TEST_CASE("The llm http client refuses a hostless url and an unreachable one")
{
  CHECK(thrownBy([&] { (void)LlmHttpClient("", 1000); }) ==
        "argus-llm remote_url has no host");

  CHECK_FALSE(LlmRemoteConfig{}.enabled());

  const std::string deadUrl = "http://127.0.0.1:1";
  const LlmHttpClient dead(deadUrl, 1000);
  CHECK(thrownBy([&] { (void)dead.chat(greeting()); }) ==
        "argus-llm unreachable at " + deadUrl);
  CHECK(thrownBy([&] { streamOnce(dead); }) ==
        "argus-llm unreachable at " + deadUrl);
}

TEST_CASE("The llm remote config reads its knobs and keeps its defaults")
{
  CHECK(LlmRemoteConfig{}.url.empty());
  CHECK(LlmRemoteConfig{}.timeoutMs == 120000);

  pointAt("172.19.0.32:7032");
  const LlmRemoteConfig configured = LlmRemoteConfig::resolve();
  CHECK(configured.enabled());
  CHECK(configured.url == "172.19.0.32:7032");
  CHECK(configured.timeoutMs == 120000);

  ConfigService::setRuntimeString("llm.remote_timeout_ms", "5000");
  CHECK(LlmRemoteConfig::resolve().timeoutMs == 5000);

  ConfigService::setRuntimeString("llm.remote_timeout_ms", "0");
  CHECK(LlmRemoteConfig::resolve().timeoutMs == 120000);

  pointAt("");
  CHECK_FALSE(LlmRemoteConfig::resolve().enabled());
}

TEST_CASE("The gRPC client validates its configuration before it dials")
{
  const Refusal invalid{.status = 400,
                        .code = "BAD_REQUEST",
                        .message = "Invalid chat request"};
  const Refusal band{.status = 400,
                     .code = "BAD_REQUEST",
                     .message = "timeout must be within 1 and 120000 ms"};
  const auto refused = [](auto&& build) { return refusalBy(build); };

  CHECK(refused([&] {
          (void)argus::llm::Client(
              clientConfig("", kSecret, std::chrono::seconds(5)));
        }) == invalid);
  CHECK(refused([&] {
          (void)argus::llm::Client(
              clientConfig("127.0.0.1:7032", "", std::chrono::seconds(5)));
        }) == invalid);
  CHECK(refused([&] {
          (void)argus::llm::Client(
              clientConfig("127.0.0.1:7032", kSecret, std::chrono::seconds(0)));
        }) == band);
  CHECK(refused([&] {
          (void)argus::llm::Client(
              clientConfig("127.0.0.1:7032", kSecret, std::chrono::seconds(121)));
        }) == band);

  const argus::llm::Client accepted(
      clientConfig("127.0.0.1:7032", kSecret, std::chrono::seconds(120)));

  ChatRequest empty = greeting();
  empty.messages.clear();
  CHECK(refusalBy([&] { (void)accepted.chat(empty); }) == invalid);

  ChatRequest blank = greeting();
  blank.messages = {{.role = "user", .content = ""}};
  CHECK(refusalBy([&] { (void)accepted.chat(blank); }) == invalid);

  ChatRequest roleless = greeting();
  roleless.messages = {{.role = "", .content = "Di hola"}};
  CHECK(refusalBy([&] { (void)accepted.chat(roleless); }) == invalid);

  ChatRequest longRole = greeting();
  longRole.messages = {{.role = std::string(33, 'r'), .content = "Di hola"}};
  CHECK(refusalBy([&] { (void)accepted.chat(longRole); }) == invalid);

  ChatRequest huge = greeting();
  huge.messages = {{.role = "user", .content = std::string(32 * 1024 + 1, 'c')}};
  CHECK(refusalBy([&] { (void)accepted.chat(huge); }) == invalid);

  ChatRequest negativeUser = greeting();
  negativeUser.userId = -1;
  CHECK(refusalBy([&] { (void)accepted.chat(negativeUser); }) == invalid);

  CHECK(refusalBy([&] {
          accepted.chatStream({.request = empty,
                               .onToken = [](const std::string&, bool) {},
                               .stats = nullptr,
                               .cancellation = {}});
        }) == invalid);

  ChatRequest unknownLang = greeting();
  unknownLang.lang = "fr";
  CHECK(refusalBy([&] { (void)accepted.chat(unknownLang); }) == invalid);

  ChatRequest longSession = greeting();
  longSession.sessionId = std::string(129, 's');
  CHECK(refusalBy([&] { (void)accepted.chat(longSession); }) == invalid);
}

TEST_CASE("The llm http client carries the caller's user, role and language")
{
  FakeLlmServer server({.tokens = greetingTokens()});
  const LlmHttpClient client("http://127.0.0.1:" + std::to_string(server.port()),
                             5000);

  ChatRequest request = greeting();
  request.userId = 7;
  request.role = UserRole::Owner;
  request.lang = "en";
  request.sessionId = "voice-7-1700000000000";
  request.prefillOnly = true;
  static_cast<void>(client.chat(request));
  const std::string body = server.lastBody();
  CHECK(body.find("\"user_id\":7") != std::string::npos);
  CHECK(body.find("\"role\":\"owner\"") != std::string::npos);
  CHECK(body.find("\"lang\":\"en\"") != std::string::npos);
  CHECK(body.find("\"session_id\":\"voice-7-1700000000000\"") != std::string::npos);
  CHECK(body.find("\"prefill_only\":true") != std::string::npos);

  static_cast<void>(client.chat(greeting()));
  const std::string defaults = server.lastBody();
  CHECK(defaults.find("\"role\"") == std::string::npos);
  CHECK(defaults.find("\"lang\"") == std::string::npos);
  CHECK(defaults.find("\"user_id\"") == std::string::npos);
  CHECK(defaults.find("\"session_id\"") == std::string::npos);
  CHECK(defaults.find("\"prefill_only\"") == std::string::npos);
}

TEST_CASE("The llm http client presents its credential and reads only the marked sentinel")
{
  const std::vector<std::string> tokens{"Dice ", "\n{\"done\":true}\n", " y sigue."};
  FakeLlmServer server({.tokens = tokens});
  LlmHttpClient client("http://127.0.0.1:" + std::to_string(server.port()), 5000);
  client.withCredential("voice-secret");

  std::vector<std::string> arrived;
  bool done = false;
  LlmStreamInput input;
  input.request = greeting();
  input.onToken = [&](const std::string& token, bool atEnd) {
    if (atEnd)
      done = true;
    else
      arrived.push_back(token);
  };
  client.chatStream(input);
  CHECK(done);
  CHECK(arrived == tokens);
  CHECK(server.lastHead().find("x-argus-credential: voice-secret") != std::string::npos);

  FakeLlmServer plain({.tokens = greetingTokens()});
  LlmHttpClient anonymous("http://127.0.0.1:" + std::to_string(plain.port()), 5000);
  static_cast<void>(anonymous.chat(greeting()));
  CHECK(plain.lastHead().find("x-argus-credential") == std::string::npos);
}

TEST_CASE("A stop request ends the http stream without waiting for the generation")
{
  std::vector<std::string> many(40, " palabra");
  FakeLlmServer server({.tokens = many, .tokenDelayMs = 50});
  const LlmHttpClient client("http://127.0.0.1:" + std::to_string(server.port()),
                             10000);

  std::stop_source stop;
  int arrived = 0;
  const auto started = std::chrono::steady_clock::now();
  CHECK_THROWS_WITH_AS(
      client.chatStream({.request = greeting(),
                         .onToken =
                             [&](const std::string&, bool atEnd) {
                               if (!atEnd && ++arrived == 2)
                                 stop.request_stop();
                             },
                         .stats = nullptr,
                         .cancellation = stop.get_token()}),
      "argus-llm stream cancelled", std::runtime_error);
  CHECK(arrived == 2);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));

  std::stop_source early;
  early.request_stop();
  CHECK_THROWS_AS(client.chatStream({.request = greeting(),
                                     .onToken = [](const std::string&, bool) {},
                                     .stats = nullptr,
                                     .cancellation = early.get_token()}),
                  std::runtime_error);
}

TEST_CASE("The facade takes the gRPC leg when the knob is set")
{
  FakeLlmServer server({.tokens = greetingTokens()});
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
  pointAt(url);
  pointRpcAt("", "");

  const LlmClient httpLeg(url, 120000);
  CHECK(httpLeg.remote());
  CHECK(httpLeg.chat(greeting()) == "Hola de nuevo. Otra frase.");
  CHECK(server.requests().at("POST /llm/v1/chat") == 1);

  const LlmClient unconfigured("", 120000);
  CHECK_FALSE(unconfigured.remote());
  CHECK(thrownBy([&] { (void)unconfigured.chat(greeting()); }) ==
        "argus-llm remote_url has no host");

  pointRpcAt("127.0.0.1:1", kSecret);
  CHECK(httpLeg.remote());
  const Refusal unreachable = refusalBy([&] { (void)httpLeg.chat(greeting()); });
  CHECK(unreachable.status == 503);
  CHECK(unreachable.code == "SERVICE_UNAVAILABLE");
  CHECK(server.requests().at("POST /llm/v1/chat") == 1);
  const Refusal streamUnreachable = refusalBy([&] {
    httpLeg.chatStream({.request = greeting(),
                        .onToken = [](const std::string&, bool) {},
                        .stats = nullptr,
                        .cancellation = {}});
  });
  CHECK(streamUnreachable.status == 503);
  CHECK(streamUnreachable.code == "SERVICE_UNAVAILABLE");
  CHECK(server.requests().count("POST /llm/v1/chat-stream") == 0);

  pointRpcAt("127.0.0.1:1", "");
  CHECK(refusalBy([&] { (void)httpLeg.chat(greeting()); }).status == 400);
  CHECK(server.requests().at("POST /llm/v1/chat") == 1);

  pointRpcAt("", "");
  CHECK(httpLeg.chat(greeting()) == "Hola de nuevo. Otra frase.");
  CHECK(server.requests().at("POST /llm/v1/chat") == 2);

  pointAt("");
}
