#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-tts-server.hxx"

#include <test-support/fake-voice-sink.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <cmath>
#include <string>
#include <vector>

namespace
{

struct FakeStt final : IVoiceStt
{
  std::string transcribe(const std::vector<float>&, int32_t) override
  {
    return "hola";
  }

  bool setLanguage(const std::string&) override { return true; }
};

struct FakeLlm final : IVoiceLlm
{
  void chatStream(const ChatRequest&, TokenCallback onToken) override
  {
    onToken("Hola.", false);
    onToken("", true);
  }
};

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("tts.remote_url", url);
}

} // namespace

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
