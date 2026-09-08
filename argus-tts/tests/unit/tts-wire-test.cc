#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/tts-controller.hxx>
#include <drogon/drogon.h>
#include <filter/valid-json/valid-json-filter.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/value.h>
#include <json/reader.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>

namespace
{
#ifndef ARGUS_TEST_TTS_MODELS_DIR
#define ARGUS_TEST_TTS_MODELS_DIR "models/tts"
#endif

constexpr const char* kScratchConfig = "tts-wire-test.toml";

struct HttpReply
{
  int status{0};
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string toLower(std::string value)
{
  for (auto& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// De-chunks a buffered chunked body into the concatenated payload.
std::string dechunk(const std::string& wire)
{
  std::string payload;
  std::string::size_type cursor = 0;
  while (cursor < wire.size()) {
    const auto lineEnd = wire.find("\r\n", cursor);
    REQUIRE(lineEnd != std::string::npos);
    const size_t size =
        std::stoul(wire.substr(cursor, lineEnd - cursor), nullptr, 16);
    if (size == 0)
      break;
    payload.append(wire, lineEnd + 2, size);
    cursor = lineEnd + 2 + size + 2;
  }
  return payload;
}

// True when the buffered chunked wire carries the terminal zero chunk.
bool hasTerminalChunk(const std::string& wire)
{
  std::string::size_type cursor = 0;
  while (cursor < wire.size()) {
    const auto lineEnd = wire.find("\r\n", cursor);
    if (lineEnd == std::string::npos)
      return false;
    const size_t size =
        std::stoul(wire.substr(cursor, lineEnd - cursor), nullptr, 16);
    if (size == 0)
      return true;
    if (wire.size() < lineEnd + 2 + size + 2)
      return false;
    cursor = lineEnd + 2 + size + 2;
  }
  return false;
}

HttpReply request(int port, const std::string& method,
                  const std::string& path, const std::string& body,
                  bool closeConnection = true)
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
  if (!body.empty())
    wire += "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n";
  if (closeConnection)
    wire += "Connection: close\r\n";
  wire += "\r\n" + body;

  size_t sent = 0;
  while (sent < wire.size()) {
    const auto n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    REQUIRE(n > 0);
    sent += static_cast<size_t>(n);
  }

  std::string data;
  char buffer[65536];
  const auto recvMore = [&data, &buffer, fd]() -> bool {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      return false;
    data.append(buffer, static_cast<size_t>(n));
    return true;
  };

  while (data.find("\r\n\r\n") == std::string::npos) {
    if (!recvMore())
      break;
  }

  const auto split = data.find("\r\n\r\n");
  REQUIRE(split != std::string::npos);
  const std::string head = data.substr(0, split);

  const auto eol = head.find("\r\n");
  const std::string statusLine = head.substr(0, eol);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  const int status =
      std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));

  std::map<std::string, std::string> headers;
  std::string::size_type cursor = eol + 2;
  while (cursor < head.size()) {
    const auto lineEnd = head.find("\r\n", cursor);
    if (lineEnd == std::string::npos)
      break;
    const auto colon = head.find(':', cursor);
    if (colon != std::string::npos)
      headers[toLower(head.substr(cursor, colon - cursor))] =
          head.substr(colon + 2, lineEnd - colon - 2);
    cursor = lineEnd + 2;
  }

  const bool chunked =
      headers.count("transfer-encoding") &&
      headers.at("transfer-encoding").find("chunked") != std::string::npos;
  const size_t bodyStart = split + 4;
  if (chunked && !closeConnection) {
    while (!hasTerminalChunk(data.substr(bodyStart))) {
      if (!recvMore())
        break;
    }
  }
  else if (headers.count("content-length") && !closeConnection) {
    const size_t length =
        static_cast<size_t>(std::stoul(headers.at("content-length")));
    while (data.size() < bodyStart + length) {
      if (!recvMore())
        break;
    }
  }
  else {
    while (recvMore())
    {
    }
  }

  ::close(fd);

  HttpReply reply;
  reply.status = status;
  reply.headers = std::move(headers);
  const std::string bufferedBody = data.substr(bodyStart);
  reply.body = chunked ? dechunk(bufferedBody) : bufferedBody;
  return reply;
}

Json::Value envelope(const HttpReply& reply)
{
  Json::Value json;
  Json::Reader reader;
  REQUIRE(reader.parse(reply.body, json));
  return json;
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

TEST_CASE("the argus-tts internal wire serves the legacy adapters")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[tts]\nthreads = 0\nquality = \"low\"\nspeed = 1.0\n"
              "max_chunk_len = 350\n[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);
  ConfigService::setRuntimeString("tts.models_dir",
                                  ARGUS_TEST_TTS_MODELS_DIR);

  ConfigService::setRuntimeString("tts.steps_cap", "16");
  CHECK(TtsService::effectiveStepsCap() == 16);
  ConfigService::setRuntimeString("tts.steps_cap", "0");
  CHECK(TtsService::effectiveStepsCap() == HardwareProbe::ttsStepsCap());
  CHECK(TtsService::effectiveStepsCap() > 0);

  TtsService::instance().init();
  REQUIRE_MESSAGE(TtsService::instance().isLoaded(),
                  "TTS engine failed to load from " ARGUS_TEST_TTS_MODELS_DIR
                  " — run scripts/setup.sh tts first");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-tts"}));
  drogon::app().registerController(std::make_shared<TtsController>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
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

  const auto health = request(port, "GET", "/health", "");
  const Json::Value healthJson = envelope(health);
  CHECK(health.status == 200);
  CHECK(healthJson["info"]["service"] == "argus-tts");

  const auto config = request(port, "GET", "/tts/v1/config", "");
  const Json::Value configJson = envelope(config);
  CHECK(config.status == 200);
  CHECK(configJson["status"].asInt() == 200);
  CHECK(configJson["info"]["sampleRate"].asInt() > 0);
  CHECK(configJson["info"]["defaultSpeed"].asDouble() > 0);
  CHECK(configJson["info"]["loaded"].asBool());

  const auto t0 = std::chrono::steady_clock::now();
  const auto synth = request(port, "POST", "/tts/v1/synthesize",
                             R"({"text":"Hola argus","lang":"es"})");
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  CHECK(synth.status == 200);
  CHECK(toLower(synth.headers.at("content-type")).find(
            "audio/x-argus-pcm-f32") != std::string::npos);
  REQUIRE(synth.headers.count("x-argus-sample-rate") == 1);
  const int wireRate = std::stoi(synth.headers.at("x-argus-sample-rate"));
  CHECK(wireRate == configJson["info"]["sampleRate"].asInt());
  CHECK(synth.body.size() % sizeof(float) == 0);
  CHECK_FALSE(synth.body.empty());
  CHECK(ms < 30000);

  const auto stream = request(port, "POST", "/tts/v1/synthesize-stream",
                              R"({"text":"Hola argus","lang":"es"})", false);
  CHECK(stream.status == 200);
  CHECK(toLower(stream.headers.at("content-type")).find(
            "audio/x-argus-pcm-f32") != std::string::npos);
  CHECK(stream.headers.at("transfer-encoding") == "chunked");
  CHECK(stream.headers.count("x-argus-sample-rate") == 1);
  const std::string& pcm = stream.body;
  CHECK(pcm.size() % sizeof(float) == 0);
  CHECK_FALSE(pcm.empty());

  const auto invalid =
      request(port, "POST", "/tts/v1/synthesize", R"({"text":""})");
  const Json::Value invalidJson = envelope(invalid);
  CHECK(invalid.status == 422);
  CHECK(invalidJson["status"].asInt() == 422);
  CHECK_FALSE(invalidJson["errors"].isNull());

  const auto badJson = request(port, "POST", "/tts/v1/synthesize", "{not json");
  CHECK(badJson.status == 400);
  CHECK(envelope(badJson)["errors"]["code"] == "BAD_REQUEST");

  const auto notFound = request(port, "GET", "/tts/v1/missing", "");
  CHECK(notFound.status == 404);
  CHECK(envelope(notFound)["errors"]["code"] == "NOT_FOUND");

  const auto notAllowed = request(port, "POST", "/tts/v1/config", "");
  CHECK(notAllowed.status == 405);
  CHECK(envelope(notAllowed)["errors"]["code"] == "METHOD_NOT_ALLOWED");

  drogon::app().quit();
  runner.join();
  TtsService::instance().shutdown();
  std::remove(kScratchConfig);
}
