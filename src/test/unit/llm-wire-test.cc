#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/llm-controller.hxx>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>

#include <llama.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/reader.h>
#include <json/value.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>

namespace
{

#ifndef ARGUS_TEST_LLM_MODELS_DIR
#define ARGUS_TEST_LLM_MODELS_DIR "models/llm"
#endif

constexpr const char* kScratchConfig = "llm-wire-test.toml";

struct HttpReply
{
  int status{0};
  std::string body;
};

HttpReply request(int port, const std::string& method,
                  const std::string& path, const std::string& body,
                  const std::string& contentType = "")
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
          0);

  std::string wire = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (!body.empty() || !contentType.empty()) {
    wire += "Content-Type: " +
            (contentType.empty() ? std::string("application/octet-stream")
                                 : contentType) +
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  wire += "Connection: close\r\n\r\n" + body;

  size_t sent = 0;
  while (sent < wire.size()) {
    const auto n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    REQUIRE(n > 0);
    sent += static_cast<size_t>(n);
  }

  std::string data;
  char buffer[65536];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      break;
    data.append(buffer, static_cast<size_t>(n));
  }
  ::close(fd);

  const auto split = data.find("\r\n\r\n");
  REQUIRE(split != std::string::npos);
  const std::string head = data.substr(0, split);

  const auto eol = head.find("\r\n");
  const std::string statusLine = head.substr(0, eol);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  HttpReply reply;
  reply.status =
      std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));
  reply.body = data.substr(split + 4);
  return reply;
}

Json::Value envelope(const HttpReply& reply)
{
  Json::Value json;
  Json::Reader reader;
  REQUIRE(reader.parse(reply.body, json));
  return json;
}

std::string chatBody(const std::string& message, int maxTokens = 0)
{
  Json::Value body(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  Json::Value entry(Json::objectValue);
  entry["role"] = "user";
  entry["content"] = message;
  messages.append(entry);
  body["messages"] = std::move(messages);
  if (maxTokens > 0)
    body["max_tokens"] = maxTokens;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, body);
}

std::string historyBody(const std::string& first, const std::string& answer,
                        const std::string& followUp)
{
  Json::Value body(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  Json::Value turn(Json::objectValue);
  turn["role"] = "user";
  turn["content"] = first;
  messages.append(turn);
  Json::Value reply(Json::objectValue);
  reply["role"] = "assistant";
  reply["content"] = answer;
  messages.append(reply);
  Json::Value next(Json::objectValue);
  next["role"] = "user";
  next["content"] = followUp;
  messages.append(next);
  body["messages"] = std::move(messages);
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, body);
}

// The chunked stream leg stays open (no Connection: close), so the reader
// de-chunks the framing itself and stops at the terminal zero chunk.
std::vector<std::string> requestStream(int port, const std::string& body)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
          0);

  std::string wire = "POST /llm/v1/chat-stream HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Content-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < wire.size()) {
    const auto n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    REQUIRE(n > 0);
    sent += static_cast<size_t>(n);
  }

  std::string data;
  char buffer[65536];
  const auto recvMore = [&] {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      return false;
    data.append(buffer, static_cast<size_t>(n));
    return true;
  };

  std::vector<std::string> chunks;
  bool ok = true;
  while (ok && data.find("\r\n\r\n") == std::string::npos)
    ok = recvMore();
  REQUIRE(ok);

  size_t cursor = data.find("\r\n\r\n") + 4;
  for (;;) {
    auto lineEnd = data.find("\r\n", cursor);
    while (lineEnd == std::string::npos && (ok = recvMore()))
      lineEnd = data.find("\r\n", cursor);
    REQUIRE(ok);
    const size_t size = std::stoul(data.substr(cursor, lineEnd - cursor),
                                   nullptr, 16);
    if (size == 0)
      break;
    const auto dataStart = lineEnd + 2;
    while (data.size() < dataStart + size + 2 && (ok = recvMore())) {
    }
    REQUIRE(ok);
    chunks.push_back(data.substr(dataStart, size));
    cursor = dataStart + size + 2;
  }
  ::close(fd);
  return chunks;
}

// The sentinel line is the exact end-of-stream marker; it is found by
// scanning for the last "\n{" whose remainder parses as a done:true JSON.
struct StreamBody
{
  std::string tokens;
  Json::Value sentinel;
  bool sentinelFound{false};
};

StreamBody joinChunks(const std::vector<std::string>& chunks)
{
  StreamBody body;
  for (const auto& chunk : chunks)
    body.tokens += chunk;

  Json::Reader reader;
  for (auto mark = body.tokens.rfind("\n{"); mark != std::string::npos;
       mark = mark > 0 ? body.tokens.rfind("\n{", mark - 1)
                       : std::string::npos) {
    std::string candidate = body.tokens.substr(mark + 1);
    if (!candidate.empty() && candidate.back() == '\n')
      candidate.pop_back();
    Json::Value json;
    if (reader.parse(candidate, json, false) && json.isObject() &&
        json.isMember("done") && json["done"].asBool()) {
      body.sentinel = json;
      body.sentinelFound = true;
      body.tokens = body.tokens.substr(0, mark);
      break;
    }
  }
  return body;
}

std::string postChat(int port, const std::string& body)
{
  return request(port, "POST", "/llm/v1/chat", body, "application/json").body;
}

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

} // namespace

TEST_CASE("the argus-llm internal wire serves the chat capacity")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[llm]\n"
           << "model_path = \"" << ARGUS_TEST_LLM_MODELS_DIR
           << "/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf\"\n"
           << "context_size = 4096\n"
           << "max_tokens = 16\n"
           << "temperature = 0.3\n"
           << "top_k = 20\n"
           << "top_p = 0.8\n"
           << "penalty_last_n = 64\n"
           << "penalty_repeat = 1.10\n"
           << "seed = 42\n"
              "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);

  // argus-llm owns the process-global llama lifecycle in main.cc; the test
  // mirrors that exact teardown order.
  llama_backend_init();

  const auto llm = std::make_shared<LlmController>();
  llm->initEngine();
  REQUIRE_MESSAGE(llm->isEngineLoaded(),
                  "LLM engine failed to load from " ARGUS_TEST_LLM_MODELS_DIR
                  " — run scripts/setup.sh first");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().setClientMaxBodySize(8 * 1024 * 1024);
  drogon::app().registerController(std::make_shared<HealthController>());
  drogon::app().registerController(llm);
  drogon::app().setExceptionHandler(AppConfig::handleException);
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });
  drogon::app().addListener("127.0.0.1", 0);

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  const auto listeners = drogon::app().getListeners();
  REQUIRE_FALSE(listeners.empty());
  const int port = listeners.front().toPort();
  REQUIRE(port > 0);

  // ── GET /health ───────────────────────────────────────────────────────
  const auto health = request(port, "GET", "/health", "");
  CHECK(health.status == 200);
  CHECK(envelope(health)["info"]["service"] == "argus-llm");

  // ── GET /llm/v1/config ────────────────────────────────────────────────
  const auto config = request(port, "GET", "/llm/v1/config", "");
  const Json::Value configJson = envelope(config);
  CHECK(config.status == 200);
  CHECK(configJson["info"]["loaded"].asBool());
  CHECK(configJson["info"]["defaultMaxTokens"].asInt() == 16);
  CHECK(configJson["info"]["defaultTemperature"].asFloat() == doctest::Approx(0.3F));
  CHECK(configJson["info"]["contextSize"].asInt64() == 4096);

  // ── POST /llm/v1/chat: frozen envelope {status, info.text, errors} ────
  const std::string firstBody = chatBody("Di exactamente: hola");
  const Json::Value firstJson = envelope({0, postChat(port, firstBody)});
  CHECK(firstJson["status"].asInt() == 200);
  CHECK(firstJson["info"].isMember("text"));
  const std::string firstText = firstJson["info"]["text"].asString();
  CHECK_FALSE(firstText.empty());

  // Fresh cache: the boot warmup leaves nothing to reuse.
  const Json::Value afterFirst = envelope(request(port, "GET", "/llm/v1/config", ""));
  CHECK(afterFirst["info"]["lastReusedTokens"].asInt() == 0);
  CHECK(afterFirst["info"]["lastPromptTokens"].asInt() > 0);
  MESSAGE("first chat: \"" << firstText << "\" prompt="
           << afterFirst["info"]["lastPromptTokens"].asInt() << " tokens");

  // KV-prefix reuse: a turn that extends the previous conversation's cached
  // prompt+answer prefix reuses it; a diverging prompt thrashes the slot.
  const std::string historyBodyWire =
      historyBody("Di exactamente: hola", firstText, "Y ahora despidete");
  const Json::Value historyJson = envelope({0, postChat(port, historyBodyWire)});
  CHECK(historyJson["status"].asInt() == 200);
  CHECK_FALSE(historyJson["info"]["text"].asString().empty());
  const Json::Value afterHistory = envelope(request(port, "GET", "/llm/v1/config", ""));
  MESSAGE("history chat reused " << afterHistory["info"]["lastReusedTokens"].asInt()
           << " of " << afterHistory["info"]["lastPromptTokens"].asInt()
           << " tokens");
  CHECK(afterHistory["info"]["lastReusedTokens"].asInt() > 0);

  const std::string divergentBody = chatBody("Cuantos dias tiene una semana?");
  const Json::Value divergentJson = envelope({0, postChat(port, divergentBody)});
  CHECK(divergentJson["status"].asInt() == 200);
  const std::string divergentText = divergentJson["info"]["text"].asString();
  CHECK_FALSE(divergentText.empty());
  const Json::Value afterDivergent = envelope(request(port, "GET", "/llm/v1/config", ""));
  CHECK(afterDivergent["info"]["lastReusedTokens"].asInt() == 0);

  // ── POST /llm/v1/chat-stream: token chunks + final sentinel line ──────
  const auto chunks = requestStream(port, divergentBody);
  const StreamBody streamed = joinChunks(chunks);
  REQUIRE(streamed.sentinelFound);
  CHECK(streamed.sentinel["prompt_tokens"].asInt() > 0);
  CHECK(streamed.sentinel["reused_tokens"].asInt() == 0);
  CHECK(streamed.sentinel["decoded_tokens"].asInt() > 0);
  CHECK_FALSE(streamed.tokens.empty());

  // Deterministic sampling (fixed seed): the streamed tokens join to the
  // exact text the chat leg produced for the same request.
  CHECK(streamed.tokens == divergentText);
  // Tokens ride their own chunks in arrival order; only the sentinel chunk
  // carries the JSON line.
  CHECK(chunks.size() > 2);
  for (size_t i = 0; i + 1 < chunks.size(); ++i)
    CHECK(chunks[i].find("\"done\"") == std::string::npos);

  // ── Validation: 400 transport shape, 422 field errors ────────────────
  const auto notJson = request(port, "POST", "/llm/v1/chat", "not json",
                               "text/plain");
  CHECK(notJson.status == 400);
  CHECK(envelope(notJson)["errors"]["code"] == "BAD_REQUEST");

  const Json::Value emptyMessages =
      envelope({0, postChat(port, "{\"messages\":[]}")});
  CHECK(emptyMessages["status"].asInt() == 422);
  CHECK(emptyMessages["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(emptyMessages["errors"]["fields"].isMember("messages"));

  const Json::Value badToken =
      envelope({0, postChat(port, chatBody("hola", 9999))});
  CHECK(badToken["status"].asInt() == 422);
  CHECK(badToken["errors"]["fields"].isMember("maxTokens"));

  // ── Frozen routing errors: 404 and 405 envelopes ─────────────────────
  const auto notFound = request(port, "GET", "/llm/v1/missing", "");
  CHECK(notFound.status == 404);
  CHECK(envelope(notFound)["errors"]["code"] == "NOT_FOUND");

  const auto notAllowed = request(port, "GET", "/llm/v1/chat", "");
  CHECK(notAllowed.status == 405);
  CHECK(envelope(notAllowed)["errors"]["code"] == "METHOD_NOT_ALLOWED");

  // ── 503 LLM_NOT_LOADED when the engine is down, then recovery ─────────
  llm->shutdownEngine();
  const auto down = postChat(port, firstBody);
  Json::Value downJson;
  REQUIRE(Json::Reader().parse(down, downJson));
  CHECK(downJson["status"].asInt() == 503);
  CHECK(downJson["errors"]["code"] == "LLM_NOT_LOADED");

  const auto streamDown = request(port, "POST", "/llm/v1/chat-stream",
                                  firstBody, "application/json");
  CHECK(streamDown.status == 503);
  CHECK(envelope(streamDown)["errors"]["code"] == "LLM_NOT_LOADED");

  llm->initEngine();
  CHECK(llm->isEngineLoaded());
  const Json::Value recovered = envelope({0, postChat(port, firstBody)});
  CHECK(recovered["status"].asInt() == 200);

  drogon::app().quit();
  runner.join();
  llm->shutdownEngine();
  llama_backend_free();
  std::remove(kScratchConfig);
}
