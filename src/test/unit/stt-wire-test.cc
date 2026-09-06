#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/stt-controller.hxx>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stt/stt-service.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/reader.h>
#include <json/value.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
#ifndef ARGUS_TEST_STT_MODELS_DIR
#define ARGUS_TEST_STT_MODELS_DIR "models/stt"
#endif

constexpr const char* kScratchConfig = "stt-wire-test.toml";

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

  HttpReply reply;
  reply.status = status;
  reply.headers = std::move(headers);
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

// 16 kHz mono s16 RIFF wav → float samples (the exact int16/32768 mapping
// the voice session applies to its WS frames).
std::vector<float> wavSamples(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  REQUIRE(data.size() > 44);
  const auto chunkAt = [&](const char* wanted)
      -> std::pair<std::string::size_type, uint32_t> {
    std::string::size_type cursor = 12;
    while (cursor + 8 <= data.size()) {
      const std::string id(data.data() + cursor, 4);
      const uint32_t size = *reinterpret_cast<const uint32_t*>(
          data.data() + cursor + 4);
      if (id == wanted)
        return {cursor + 8, size};
      cursor += 8 + size + (size & 1);
    }
    return {std::string::npos, 0};
  };
  const auto [dataStart, dataSize] = chunkAt("data");
  REQUIRE(dataStart != std::string::npos);
  std::vector<float> samples(dataSize / sizeof(int16_t));
  for (size_t i = 0; i < samples.size(); ++i) {
    int16_t raw = 0;
    std::memcpy(&raw, data.data() + dataStart + i * sizeof(int16_t),
                sizeof(raw));
    samples[i] = static_cast<float>(raw) / 32768.0F;
  }
  return samples;
}

std::string pcmBytes(const std::vector<float>& samples)
{
  std::string bytes(samples.size() * sizeof(int16_t), '\0');
  for (size_t i = 0; i < samples.size(); ++i) {
    const float clamped = std::max(-1.0F, std::min(1.0F, samples[i]));
    const long scaled = std::lround(clamped * 32768.0F);
    const auto raw =
        static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
    std::memcpy(bytes.data() + i * sizeof(int16_t), &raw, sizeof(raw));
  }
  return bytes;
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

TEST_CASE("the argus-stt internal wire serves the legacy voice session")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[stt]\nengine = \"whisper\"\nlanguage = \"es\"\n"
              "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);
  ConfigService::setRuntimeString("stt.models_dir", ARGUS_TEST_STT_MODELS_DIR);

  // The capacity boots for real: no engine, no contract.
  SttService::instance().init();
  REQUIRE_MESSAGE(SttService::instance().isLoaded(),
                  "STT engine failed to load from " ARGUS_TEST_STT_MODELS_DIR
                  " — run scripts/setup.sh stt first");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().setClientMaxBodySize(64 * 1024 * 1024);
  drogon::app().registerController(std::make_shared<HealthController>());
  drogon::app().registerController(std::make_shared<SttController>());
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

  constexpr const char* kWav =
      ARGUS_TEST_STT_MODELS_DIR "/zipformer-en/test_wavs/0.wav";

  // ── GET /health ───────────────────────────────────────────────────────────
  const auto health = request(port, "GET", "/health", "");
  CHECK(health.status == 200);
  CHECK(envelope(health)["info"]["service"] == "argus-stt");

  // ── GET /stt/v1/config ────────────────────────────────────────────────────
  const auto config = request(port, "GET", "/stt/v1/config", "");
  const Json::Value configJson = envelope(config);
  CHECK(config.status == 200);
  CHECK(configJson["info"]["loaded"].asBool());
  CHECK(configJson["info"]["defaultLanguage"] == "es");
  CHECK(configJson["info"]["language"] == "es");

  // ── POST /stt/v1/transcribe: binary s16 PCM in, envelope {text} out ──────
  const std::vector<float> samples = wavSamples(kWav);
  const std::string pcm = pcmBytes(samples);
  REQUIRE(samples.size() > 16000);

  const auto t0 = std::chrono::steady_clock::now();
  const auto transcribe =
      request(port, "POST", "/stt/v1/transcribe?lang=es", pcm,
              "audio/x-argus-pcm-s16");
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  CHECK(transcribe.status == 200);
  CHECK(toLower(transcribe.headers.at("content-type")).find(
            "application/json") != std::string::npos);
  const Json::Value transcribeJson = envelope(transcribe);
  CHECK(transcribeJson["status"].asInt() == 200);
  CHECK(transcribeJson["info"].isMember("text"));
  CHECK(ms < 30000);

  // A/B vs the pre-cutover in-process path: the same PCM through
  // SttService::transcribe produces the SAME TEXT (deterministic greedy
  // decode; audio bytes are not the acceptance, text is).
  const std::string inProcess =
      SttService::instance().transcribe(samples, 16000);
  CHECK(transcribeJson["info"]["text"].asString() == inProcess);

  // ── lang handling: "" resolves from the service stt.language config ──────
  const auto emptyLang = request(port, "POST", "/stt/v1/transcribe?lang=", pcm,
                                 "audio/x-argus-pcm-s16");
  CHECK(emptyLang.status == 200);
  CHECK(envelope(emptyLang)["info"]["text"] == inProcess);

  const auto noParam = request(port, "POST", "/stt/v1/transcribe", pcm,
                               "audio/x-argus-pcm-s16");
  CHECK(noParam.status == 200);
  CHECK(envelope(noParam)["info"]["text"] == inProcess);

  // Ruling BE: one global recognizer — a different lang rebuilds it and
  // /stt/v1/config reports the new language.
  const auto english = request(port, "POST", "/stt/v1/transcribe?lang=en", pcm,
                               "audio/x-argus-pcm-s16");
  CHECK(english.status == 200);
  const auto afterSwitch = request(port, "GET", "/stt/v1/config", "");
  CHECK(envelope(afterSwitch)["info"]["language"] == "en");

  // Unsupported lang → 422 with the frozen envelope.
  const auto french = request(port, "POST", "/stt/v1/transcribe?lang=fr", pcm,
                              "audio/x-argus-pcm-s16");
  CHECK(french.status == 422);
  CHECK(envelope(french)["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(envelope(french)["errors"]["fields"].isMember("lang"));

  // ── Body validation: 400 for transport-shape problems, 422 for empty ─────
  const auto badType = request(port, "POST", "/stt/v1/transcribe?lang=es", pcm,
                               "audio/wav");
  CHECK(badType.status == 400);
  CHECK(envelope(badType)["errors"]["code"] == "BAD_REQUEST");

  const auto oddBody = request(port, "POST", "/stt/v1/transcribe?lang=es",
                               std::string(3, '\0'), "audio/x-argus-pcm-s16");
  CHECK(oddBody.status == 400);
  CHECK(envelope(oddBody)["errors"]["code"] == "BAD_REQUEST");

  const auto emptyBody = request(port, "POST", "/stt/v1/transcribe?lang=es",
                                 "", "audio/x-argus-pcm-s16");
  CHECK(emptyBody.status == 422);
  CHECK(envelope(emptyBody)["errors"]["fields"].isMember("body"));

  // ── Frozen routing errors: 404 and 405 envelopes ─────────────────────────
  const auto notFound = request(port, "GET", "/stt/v1/missing", "");
  CHECK(notFound.status == 404);
  CHECK(envelope(notFound)["errors"]["code"] == "NOT_FOUND");

  const auto notAllowed = request(port, "POST", "/stt/v1/config", "");
  CHECK(notAllowed.status == 405);
  CHECK(envelope(notAllowed)["errors"]["code"] == "METHOD_NOT_ALLOWED");

  // ── Long audio: latency is served and logged (samples + ms at INFO) ──────
  const std::vector<float> longSamples =
      wavSamples(ARGUS_TEST_STT_MODELS_DIR "/zipformer-en/test_wavs/1.wav");
  const auto longT0 = std::chrono::steady_clock::now();
  const auto longReply =
      request(port, "POST", "/stt/v1/transcribe?lang=en", pcmBytes(longSamples),
              "audio/x-argus-pcm-s16");
  const auto longMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - longT0)
          .count();
  CHECK(longReply.status == 200);
  CHECK(longSamples.size() > 16000 * 10);
  CHECK(longMs < 60000);

  // ── 503 STT_NOT_LOADED when the engine is down ───────────────────────────
  SttService::instance().shutdown();
  const auto down = request(port, "POST", "/stt/v1/transcribe?lang=es", pcm,
                            "audio/x-argus-pcm-s16");
  CHECK(down.status == 503);
  CHECK(envelope(down)["errors"]["code"] == "STT_NOT_LOADED");
  SttService::instance().init();
  CHECK(SttService::instance().isLoaded());

  drogon::app().quit();
  runner.join();
  SttService::instance().shutdown();
  std::remove(kScratchConfig);
}
