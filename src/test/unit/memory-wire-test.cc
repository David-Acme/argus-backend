#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/memory-controller.hxx>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/reader.h>
#include <json/value.h>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "database/memory-schema.sql"
#endif
#ifndef ARGUS_TEST_MEMORY_MODELS_DIR
#define ARGUS_TEST_MEMORY_MODELS_DIR "models/memory"
#endif
#ifndef ARGUS_TEST_MEMORY_EXTRACT_MODEL
#define ARGUS_TEST_MEMORY_EXTRACT_MODEL \
    "models/extract/NuExtract-1.5-tiny-Q4_K_M.gguf"
#endif

constexpr const char* kScratchConfig = "memory-wire-test.toml";

// Fake argus-llm stand-in so the wire endpoints run against the real stack.
class FakeLlmServer
{
public:
  FakeLlmServer()
  {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) ==
            0);
    port_ = ntohs(addr.sin_port);
    REQUIRE(::listen(fd_, 8) == 0);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeLlmServer()
  {
    stop_ = true;
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    thread_.join();
  }

  int port() const { return port_; }

private:
  void serve()
  {
    while (!stop_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0)
        break;
      std::string request;
      char buffer[4096];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0)
          break;
        request.append(buffer, static_cast<size_t>(n));
      }
      const std::string body =
          R"({"status":200,"info":{"text":"resumen de prueba"},"errors":{}})";
      std::string reply = "HTTP/1.1 200 OK\r\n";
      reply += "Content-Type: application/json\r\n";
      reply += "Content-Length: " + std::to_string(body.size()) + "\r\n";
      reply += "Connection: close\r\n\r\n" + body;
      size_t sent = 0;
      while (sent < reply.size()) {
        const auto n = ::send(client, reply.data() + sent,
                              reply.size() - sent, 0);
        if (n <= 0)
          break;
        sent += static_cast<size_t>(n);
      }
      ::close(client);
    }
  }

  int fd_{-1};
  int port_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

struct HttpReply
{
  int status{0};
  std::string body;
};

HttpReply request(int port, const std::string& method,
                  const std::string& path, const std::string& body)
{
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int fd = -1;
  bool connected = false;
  for (int attempt = 0; attempt < 10 && !connected; ++attempt) {
    if (fd >= 0)
      ::close(fd);
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    connected = fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                                     sizeof(addr)) == 0;
    if (!connected)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  REQUIRE(connected);

  std::string wire = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (!body.empty()) {
    wire += "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n";
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

Json::Value parseJson(const std::string& text)
{
  Json::Value json;
  Json::Reader reader;
  REQUIRE(reader.parse(text, json));
  return json;
}

Json::Value postJson(int port, const std::string& path,
                     const Json::Value& body)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string payload = Json::writeString(builder, body);
  return envelope(request(port, "POST", path, payload));
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

TEST_CASE("the argus-memory internal wire serves the memory capacity")
{
  const FakeLlmServer llm;

  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[server]\nport = 0\n"
           << "[database]\nfile = \"/tmp/f46-memory-wire/memory.db\"\n"
           << "[memory]\n"
           << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
           << "catalog_person_table = \"catalog_person\"\n"
           << "catalog_camera_table = \"catalog_camera\"\n"
           << "catalog_zone_table = \"catalog_zone\"\n"
           << "catalog_stream_table = \"catalog_stream\"\n"
           << "create_face_vec = false\n"
           << "queue_bound = 64\n"
           << "embedding_model = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
           << "/model.onnx\"\n"
           << "embedding_tokenizer = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
           << "/tokenizer.json\"\n"
           << "embedding_preload = true\n"
           << "[extract]\nmodel_path = \"" << ARGUS_TEST_MEMORY_EXTRACT_MODEL
           << "\"\n"
           << "[llm]\nremote_url = \"127.0.0.1:" << llm.port() << "\"\n"
           << "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);
  std::filesystem::create_directories("/tmp/f46-memory-wire");
  std::remove("/tmp/f46-memory-wire/memory.db");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().setClientMaxBodySize(8 * 1024 * 1024);
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-memory"}));
  const auto memory = std::make_shared<MemoryController>();
  drogon::app().registerController(memory);

  memory->initStack();
  REQUIRE(memory->isStackLoaded());

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

  {
    sqlite3* scratch = nullptr;
    REQUIRE(sqlite3_open_v2("/tmp/f46-memory-wire/memory.db", &scratch,
                            SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    SqliteStmt stmt;
    auto tableCount = [&](const char* name) {
      REQUIRE(stmt.prepare(scratch,
                           "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                           "'table' AND name = ?"));
      stmt.bindText(1, name);
      REQUIRE(stmt.step() == SQLITE_ROW);
      const int64_t count = stmt.columnInt64(0);
      stmt.finalize();
      return count;
    };
    CHECK(tableCount("memory_vec") == 1);
    CHECK(tableCount("memory_fact") == 1);
    CHECK(tableCount("memory_episode") == 1);
    CHECK(tableCount("memory_entity") == 1);
    CHECK(tableCount("memory_alias") == 1);
    CHECK(tableCount("memory_edge") == 1);
    CHECK(tableCount("memory_procedure") == 1);
    CHECK(tableCount("catalog_person") == 1);
    CHECK(tableCount("catalog_camera") == 1);
    CHECK(tableCount("face_vec") == 0);
    sqlite3_close(scratch);
  }

  const auto health = request(port, "GET", "/health", "");
  CHECK(health.status == 200);
  CHECK(envelope(health)["info"]["service"] == "argus-memory");

  Json::Value remember(Json::objectValue);
  remember["subject"] = "mi hermana";
  remember["predicate"] = "se llama";
  remember["value"] = "Ana";
  remember["type"] = "persona";
  Json::Value context(Json::objectValue);
  context["user_id"] = 1;
  context["lang"] = "es";
  remember["context"] = context;

  const Json::Value rememberJson =
      postJson(port, "/memory/v1/remember", remember);
  CHECK(rememberJson["status"].asInt() == 200);
  CHECK(rememberJson["info"]["ok"].asBool());
  const int64_t factId = rememberJson["info"]["data"]["fact_id"].asInt64();
  CHECK(factId > 0);
  CHECK(memory->service().flushPending(10000));

  Json::Value recallA(Json::objectValue);
  recallA["query"] = "mi hermana";
  recallA["context"] = context;
  const Json::Value recallAJson =
      postJson(port, "/memory/v1/recall", recallA);
  CHECK(recallAJson["status"].asInt() == 200);
  CHECK(recallAJson["info"]["ok"].asBool());
  const std::string recallText = recallAJson["info"]["output"].asString();
  CHECK(recallText.find("se llama ana") != std::string::npos);

  Json::Value recallB(Json::objectValue);
  recallB["query"] = "mi hermana";
  Json::Value contextB(Json::objectValue);
  contextB["user_id"] = 2;
  contextB["lang"] = "es";
  recallB["context"] = contextB;
  const Json::Value recallBJson =
      postJson(port, "/memory/v1/recall", recallB);
  CHECK(recallBJson["status"].asInt() == 200);
  CHECK_FALSE(recallBJson["info"]["ok"].asBool());

  Json::Value procedure(Json::objectValue);
  procedure["goal"] = "encender las luces del salon";
  const Json::Value procedureJson =
      postJson(port, "/memory/v1/procedure-run", procedure);
  CHECK(procedureJson["status"].asInt() == 200);
  CHECK_FALSE(procedureJson["info"]["ok"].asBool());
  CHECK(procedureJson["info"]["output"].asString().find(
            "no hay un procedimiento conocido") != std::string::npos);

  Json::Value forget(Json::objectValue);
  forget["fact_id"] = factId;
  const Json::Value forgetJson = postJson(port, "/memory/v1/forget", forget);
  CHECK(forgetJson["status"].asInt() == 200);
  CHECK(forgetJson["info"]["ok"].asBool());

  const Json::Value recallGoneJson =
      postJson(port, "/memory/v1/recall", recallA);
  CHECK_FALSE(recallGoneJson["info"]["ok"].asBool());

  Json::Value durable(Json::objectValue);
  durable["transcript"] =
      "user: buenas tardes\nassistant: hola, en que te ayudo\n"
      "user: donde esta mi hermana?\nassistant: esta en el salon\n"
      "user: mi hermana se llama Ana\nassistant: anotado";
  durable["lang"] = "es";
  const Json::Value durableJson =
      postJson(port, "/memory/v1/durable-transcript", durable);
  CHECK(durableJson["status"].asInt() == 200);
  const std::string durableText = durableJson["info"]["text"].asString();
  CHECK(durableText.find("user: buenas tardes") != std::string::npos);
  CHECK(durableText.find("assistant: hola, en que te ayudo") !=
        std::string::npos);
  CHECK(durableText.find("donde esta mi hermana") == std::string::npos);
  CHECK(durableText.find("esta en el salon") == std::string::npos);
  CHECK(durableText.find("user: mi hermana se llama Ana") !=
        std::string::npos);
  CHECK(durableText.find("assistant: anotado") != std::string::npos);

  const Json::Value badType = postJson(
      port, "/memory/v1/remember",
      parseJson(R"({"subject":"s","predicate":"p","value":"v",
                   "type":"otro"})"));
  CHECK(badType["status"].asInt() == 422);
  CHECK(badType["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(badType["errors"]["fields"].isMember("type"));

  const Json::Value emptyRecall =
      postJson(port, "/memory/v1/recall", parseJson(R"({"query": ""})"));
  CHECK(emptyRecall["status"].asInt() == 422);
  CHECK(emptyRecall["errors"]["fields"].isMember("query"));

  const Json::Value badForget =
      postJson(port, "/memory/v1/forget", parseJson(R"({"fact_id": 0})"));
  CHECK(badForget["status"].asInt() == 422);
  CHECK(badForget["errors"]["fields"].isMember("fact_id"));

  CHECK(request(port, "POST", "/memory/v1/recall", "not json").status ==
        400);

  Json::Value unknown(Json::objectValue);
  unknown["query"] = "algo";
  CHECK(postJson(port, "/memory/v1/unknown", unknown)["status"].asInt() ==
        404);
  CHECK(request(port, "GET", "/memory/v1/recall", "").status == 405);

  const auto dormant = std::make_shared<MemoryController>();
  Json::Value dormantBody;
  Json::Reader dormantReader;
  REQUIRE(dormantReader.parse(R"({"query": "x"})", dormantBody));
  auto req = drogon::HttpRequest::newHttpJsonRequest(dormantBody);
  req->setPath("/memory/v1/recall");
  req->setMethod(drogon::Post);
  const auto notLoaded = drogon::sync_wait(dormant->recall(req));
  CHECK(notLoaded->getStatusCode() == drogon::k503ServiceUnavailable);
  Json::Value notLoadedJson;
  Json::Reader reader;
  REQUIRE(reader.parse(std::string(notLoaded->getBody()), notLoadedJson));
  CHECK(notLoadedJson["errors"]["code"] == "MEMORY_NOT_LOADED");

  Json::Value capture(Json::objectValue);
  capture["text"] = "recuerda que mi hermana se llama Ana";
  capture["user_id"] = 3;
  capture["lang"] = "es";
  const Json::Value captureJson = postJson(port, "/memory/v1/capture", capture);
  CHECK(captureJson["status"].asInt() == 200);
  CHECK(captureJson["info"]["outcome"].asString() == "stored");
  CHECK(captureJson["info"]["fact_id"].asInt64() > 0);

  Json::Value compact(Json::objectValue);
  compact["user_id"] = 3;
  compact["transcript"] =
      "user: mi hermana se llama Ana\nassistant: anotado";
  compact["lang"] = "es";
  const Json::Value compactJson = postJson(port, "/memory/v1/compact", compact);
  CHECK(compactJson["status"].asInt() == 200);
  CHECK(compactJson["info"]["queued"].asBool());

  drogon::app().quit();
  runner.join();
  memory->shutdownStack();
  std::remove(kScratchConfig);
}
