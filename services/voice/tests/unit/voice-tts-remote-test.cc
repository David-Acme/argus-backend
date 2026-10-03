#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-tts-server.hxx"

#include <test-support/fake-voice-sink.hxx>
#include <config/config-service.hxx>

#include <atomic>
#include <cmath>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct FakeStt final : IVoiceStt
{
  std::string transcribe(const VoiceTranscribeInput&) override
  {
    return "hola";
  }
};

struct FakeLlm final : IVoiceLlm
{
  void chatStream(LlmStreamInput input) override
  {
    input.onToken("Hola.", false);
    input.onToken("", true);
  }
};

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("tts.remote_url", url);
}

}

TEST_CASE("RemoteVoiceTts serves the IVoiceTts seam over the argus-tts wire")
{
  FakeTtsServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  RemoteVoiceTts adapter;
  CHECK(adapter.defaultSpeed() == doctest::Approx(1.25F));
  CHECK(adapter.sampleRate() == 22050);

  std::vector<float> pcm;
  int chunks = 0;
  adapter.synthesizeStream({.text = "hola",
                            .lang = TtsLang::ES,
                            .voiceId = "M3",
                            .quality = TtsQuality::Auto,
                            .speed = 1.05F},
                           [&](const std::vector<float>& chunk) {
                             pcm.insert(pcm.end(), chunk.begin(), chunk.end());
                             ++chunks;
                           });
  CHECK(chunks == 2);
  REQUIRE(pcm.size() == 128);
  CHECK(std::fabs(pcm[0] - 0.25F) < 1e-6F);

  const auto requests = server.requests();
  CHECK(requests.at("POST /tts/v1/synthesize-stream") == 1);
  CHECK(requests.at("GET /tts/v1/config") == 2);

  pointAt("");
}

TEST_CASE("RemoteVoiceTts propagates cancellation through HTTP")
{
  FakeTtsServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVoiceTts adapter;
  std::stop_source stop;
  int chunks = 0;
  TtsRemoteStreamInput input{
      .request = {.text = "hello", .lang = TtsLang::EN, .voiceId = "M3",
                  .quality = TtsQuality::Auto, .speed = 1.0F},
      .onChunk = [&](const std::vector<float>&) {
        ++chunks;
        stop.request_stop();
      },
      .cancellation = stop.get_token()};
  CHECK_THROWS(adapter.synthesizeStream(input));
  CHECK(chunks == 1);
  CHECK_THROWS(adapter.synthesizeStream(input));
  CHECK(chunks == 1);
  pointAt("");
}

TEST_CASE("An externally cancelled stream interrupts a blocked HTTP receive")
{
  FakeTtsServer server(200, {{.delay = std::chrono::milliseconds(0)},
                             {.delay = std::chrono::milliseconds(1500)}});
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVoiceTts adapter;
  std::stop_source stop;
  std::atomic<int> chunks{0};
  std::atomic<bool> finished{false};
  std::chrono::steady_clock::time_point started;
  std::jthread worker([&] {
    TtsRemoteStreamInput input{
        .request = {.text = "hello", .lang = TtsLang::EN, .voiceId = "M3",
                    .quality = TtsQuality::Auto, .speed = 1.0F},
        .onChunk = [&](const std::vector<float>&) { ++chunks; },
        .cancellation = stop.get_token()};
    started = std::chrono::steady_clock::now();
    try {
      adapter.synthesizeStream(input);
    }
    catch (const std::exception&) {
    }
    finished.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (chunks.load() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(chunks.load() == 1);
  stop.request_stop();
  const auto cancelledAt = std::chrono::steady_clock::now();
  while (!finished.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  CHECK(finished.load());
  CHECK(std::chrono::steady_clock::now() - cancelledAt <
        std::chrono::milliseconds(1000));
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500));
  worker.join();
  pointAt("");
}

TEST_CASE("Cancellation at the final chunk still reports failure, not success")
{
  FakeTtsServer server(200, {{.delay = std::chrono::milliseconds(0)},
                             {.delay = std::chrono::milliseconds(0), .stop = true}});
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVoiceTts adapter;
  std::stop_source stop;
  std::atomic<int> chunks{0};
  TtsRemoteStreamInput input{
      .request = {.text = "hello", .lang = TtsLang::EN, .voiceId = "M3",
                  .quality = TtsQuality::Auto, .speed = 1.0F},
      .onChunk = [&](const std::vector<float>&) {
        ++chunks;
        stop.request_stop();
      },
      .cancellation = stop.get_token()};
  bool cancelled = false;
  try {
    adapter.synthesizeStream(input);
  }
  catch (const std::exception& error) {
    cancelled = std::string(error.what()).find("cancel") != std::string::npos;
  }
  CHECK(chunks.load() == 1);
  CHECK(cancelled);
  pointAt("");
}

TEST_CASE("A pre-stopped token never reaches the remote")
{
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int deadPort = ntohs(addr.sin_port);
  ::close(probe);
  pointAt("http://127.0.0.1:" + std::to_string(deadPort));

  RemoteVoiceTts adapter;
  std::stop_source stop;
  stop.request_stop();
  TtsRemoteStreamInput input{
      .request = {.text = "hello", .lang = TtsLang::EN, .voiceId = "M3",
                  .quality = TtsQuality::Auto, .speed = 1.0F},
      .onChunk = [](const std::vector<float>&) {},
      .cancellation = stop.get_token()};
  CHECK_THROWS(adapter.synthesizeStream(input));
  CHECK_THROWS(adapter.defaultSpeed(stop.get_token()));
  CHECK_THROWS(adapter.sampleRate(stop.get_token()));
  pointAt("");
}

TEST_CASE("An externally cancelled config fetch interrupts a blocked receive")
{
  FakeTtsServer server;
  server.delayConfig(std::chrono::milliseconds(1500));
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVoiceTts adapter;
  std::stop_source stop;
  std::atomic<bool> finished{false};
  std::jthread worker([&] {
    try {
      adapter.defaultSpeed(stop.get_token());
    }
    catch (const std::exception&) {
    }
    finished.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.request_stop();
  const auto cancelledAt = std::chrono::steady_clock::now();
  const auto deadline = cancelledAt + std::chrono::seconds(2);
  while (!finished.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  CHECK(finished.load());
  CHECK(std::chrono::steady_clock::now() - cancelledAt <
        std::chrono::milliseconds(1000));
  worker.join();
  pointAt("");
}

TEST_CASE("A live token with stop state still fetches config")
{
  FakeTtsServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVoiceTts adapter;
  std::stop_source stop;
  CHECK(stop.get_token().stop_possible());
  CHECK_FALSE(stop.get_token().stop_requested());
  CHECK(adapter.defaultSpeed(stop.get_token()) == doctest::Approx(1.25F));
  CHECK(adapter.sampleRate(stop.get_token()) == 22050);
  pointAt("");
}

TEST_CASE("The voice session greeting flows through the remote adapter")
{
  FakeTtsServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  RemoteVoiceTts tts;

  int chunks = 0;
  tts.synthesizeStream({.text = "Hola, soy Argus, tu asistente local.",
                        .lang = TtsLang::ES,
                        .voiceId = "M3",
                        .quality = TtsQuality::Auto,
                        .speed = 1.05F},
                       [&](const std::vector<float>&) { ++chunks; });
  CHECK(chunks == 2);

  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  session.start(sink, voiceIdentity);

  CHECK(waitFor([&] {
    return sink.hasType("voice:assistant");
  }));
  CHECK_FALSE(sink.of(true).empty());

  const auto requests = server.requests();
  CHECK(requests.at("POST /tts/v1/synthesize-stream") >= 2);

  session.stop(sink);
  pointAt("");
}

TEST_CASE("An unreachable argus-tts degrades the speak leg, not the session")
{
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int deadPort = ntohs(addr.sin_port);
  ::close(probe);
  pointAt("http://127.0.0.1:" + std::to_string(deadPort));

  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  RemoteVoiceTts tts;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  session.start(sink, voiceIdentity);

  CHECK(waitFor([&] {
    return sink.hasType("voice:assistant");
  }));
  CHECK(sink.of(true).empty());

  session.stop(sink);
  pointAt("");
}
