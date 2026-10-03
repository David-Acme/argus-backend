#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-stt-server.hxx"

#include <test-support/fake-voice-sink.hxx>
#include <config/config-service.hxx>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct FakeTts final : IVoiceTts
{
  float defaultSpeed(std::stop_token = {}) const override { return 1.25F; }
  int sampleRate(std::stop_token = {}) const override { return 22050; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    input.onChunk(std::vector<float>(512, 0.25F));
  }
};

struct FakeLlm final : IVoiceLlm
{
  void chatStream(LlmStreamInput input) override
  {
    input.onToken("Hola de nuevo.", false);
    input.onToken("", true);
  }
};

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("stt.remote_url", url);
}

}

struct VoiceSessionTestAccess
{
  static std::shared_ptr<VoiceSessionService::Session>
  sessionOf(VoiceSessionService& service, VoiceSessionSink& sink)
  {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.sessions_.at(&sink);
  }

  struct RunTurnInput
  {
    VoiceSessionService& service;
    VoiceSessionService::Session& session;
    const std::vector<float>& samples;
  };

  static void runTurn(const RunTurnInput& input)
  {
    input.service.processTurn(input.session, input.samples);
  }
};

TEST_CASE("RemoteVoiceStt serves the IVoiceStt seam over the argus-stt wire")
{
  FakeSttServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  RemoteVoiceStt adapter;
  const std::vector<float> samples(1600, 0.1F);

  const auto transcribe = [&adapter, &samples](const std::string& language,
                                                int32_t sampleRate) {
    return adapter.transcribe(
        {.samples = samples, .sampleRate = sampleRate, .language = language});
  };

  CHECK(transcribe("", 16000) == "hola default");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=") == 1);

  CHECK(transcribe("es", 16000) == "hola es");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 1);

  CHECK(transcribe("en", 16000) == "hola en");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=en") == 1);

  CHECK(transcribe("es", 16000) == "hola es");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 2);

  const auto before = server.requests();
  CHECK_THROWS_AS(transcribe("fr", 16000), std::invalid_argument);
  CHECK(server.requests() == before);

  CHECK_THROWS_AS(transcribe("en", 44100), std::runtime_error);

  FakeSttServer secondServer;
  pointAt("http://127.0.0.1:" + std::to_string(secondServer.port()));
  CHECK(transcribe("en", 16000) == "hola en");
  CHECK(secondServer.requests().at("POST /stt/v1/transcribe?lang=en") == 1);
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=en") == 1);

  CHECK(server.lastBodySize() == samples.size() * sizeof(int16_t));

  pointAt("");
}

TEST_CASE("The voice session transcribes through the remote adapter")
{
  FakeSttServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  RemoteVoiceStt stt;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  session.start(sink, voiceIdentity);
  CHECK(waitFor([&] {
    return sink.hasType("voice:assistant");
  }));

  CHECK(server.requests().empty());

  const size_t chunksBefore = sink.of(true).size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 1);
  argus::voice::v1::ServerFrame sttFrame;
  for (const auto& frame : sink.snapshot())
    if (frame.has_stt())
      sttFrame = frame;
  CHECK(sttFrame.stt().text() == "hola es");
  CHECK(sink.of(true).size() > chunksBefore);

  session.stop(sink);
  pointAt("");
}

TEST_CASE("An unreachable argus-stt degrades the turn, not the session")
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

  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  RemoteVoiceStt stt;
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

  const size_t framesBefore = sink.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK_FALSE(sink.hasType("voice:stt"));
  argus::voice::v1::ServerFrame event;
  bool eventFound = false;
  for (const auto& frame : sink.snapshot())
    if (frame.has_event() && frame.event().because() == "stt_failed") {
      event = frame;
      eventFound = true;
    }
  CHECK(eventFound);

  sess->history.clear();
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});
  CHECK(sink.size() > framesBefore);

  session.stop(sink);
  pointAt("");
}
