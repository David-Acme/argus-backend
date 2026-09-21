#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fake-llm-server.hxx"

#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <exception>
#include <llm/details/llm-remote.hxx>
#include <llm/llm-service.hxx>
#include <string>
#include <vector>

namespace
{

// The timeout a shipped config carries; pointAt restores it after every case
// so no case leaks a knob into its neighbours.
constexpr const char* kDefaultTimeoutMs = "120000";

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("llm.remote_url", url);
  ConfigService::setRuntimeString("llm.remote_timeout_ms", kDefaultTimeoutMs);
}

// Returns what `call` threw, or "" when it returned: a case pins the message,
// not merely that something was thrown.
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
  request.messages = {{"user", "Di hola"}};
  request.maxTokens = 16;
  return request;
}

std::vector<std::string> greetingTokens()
{
  return {"Hola", " de", " nuevo", ".", " Otra", " frase", "."};
}

// One streamed call with a no-op sink: the case asserts what it threw.
void streamOnce(const LlmHttpClient& client)
{
  LlmStreamInput input;
  input.request = greeting();
  input.onToken = [](const std::string&, bool) {};
  client.chatStream(input);
}

} // namespace

TEST_CASE("The llm client chats and streams over the argus-llm wire")
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

  // A coalescing server sends the whole generation in one chunk; the sentinel
  // is still stripped out of it.
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

  // The shipped configs spell the endpoint without a scheme.
  const LlmHttpClient bare("127.0.0.1:" + std::to_string(server.port()),
                           config.timeoutMs);
  CHECK(bare.chat(greeting()) == "Hola de nuevo. Otra frase.");
  CHECK(server.requests().at("POST /llm/v1/chat") == 2);

  pointAt("");
}

TEST_CASE("The llm client maps a refusal and a truncated stream into errors")
{
  // Both servers below are dialled through an explicit url, so no config
  // knob is consulted here.
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

TEST_CASE("The llm client refuses a hostless url and an unreachable one")
{
  CHECK(thrownBy([&] { (void)LlmHttpClient("", 1000); }) ==
        "argus-llm remote_url has no host");

  // An unset knob is a disabled config, never a silent dial to port 80.
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

  // A non-positive timeout keeps the documented default.
  ConfigService::setRuntimeString("llm.remote_timeout_ms", "0");
  CHECK(LlmRemoteConfig::resolve().timeoutMs == 120000);

  pointAt("");
  CHECK_FALSE(LlmRemoteConfig::resolve().enabled());
}
