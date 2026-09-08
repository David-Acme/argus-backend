#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-stt-server.hxx"

#include <test-support/fake-voice-sink.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <string>
#include <vector>

namespace
{

struct FakeTts final : IVoiceTts
{
  float defaultSpeed() const override { return 1.25F; }
  int sampleRate() const override { return 22050; }

  void synthesizeStream(const TtsRequest&, TtsChunkCallback onChunk) override
  {
    onChunk(std::vector<float>(512, 0.25F));
  }
};

struct FakeLlm final : IVoiceLlm
{
  void chatStream(const ChatRequest&, TokenCallback onToken) override
  {
    onToken("Hola de nuevo.", false);
    onToken("", true);
  }
};

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("stt.remote_url", url);
}

} // namespace

// Must match the friend declaration inside VoiceSessionService (global scope).
struct VoiceSessionTestAccess
{
  static std::shared_ptr<VoiceSessionService::Session>
  sessionOf(VoiceSessionService& service, VoiceSessionSink& sink)
  {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.sessions_.at(&sink);
  }

  static void runTurn(VoiceSessionService& service,
                      VoiceSessionService::Session& session,
                      const std::vector<float>& samples)
  {
    service.processTurn(session, samples);
  }
};

TEST_CASE("RemoteVoiceStt serves the IVoiceStt seam over the argus-stt wire")
{
  FakeSttServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));

  RemoteVoiceStt adapter;
  const std::vector<float> samples(1600, 0.1F);

  // No setLanguage yet: the empty lang rides the wire and the service
  // resolves it from its own stt.language config.
  CHECK(adapter.transcribe(samples, 16000) == "hola default");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=") == 1);

  CHECK(adapter.setLanguage("es"));
  CHECK(adapter.transcribe(samples, 16000) == "hola es");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 1);

  CHECK(adapter.setLanguage("en"));
  CHECK(adapter.transcribe(samples, 16000) == "hola en");
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=en") == 1);

  // The same accepted set as SttService::setLanguage, rejected locally
  // without a wire call.
  const auto before = server.requests();
  CHECK_FALSE(adapter.setLanguage("fr"));
  CHECK(server.requests() == before);

  // The wire is 16 kHz mono by contract.
  CHECK_THROWS_AS(adapter.transcribe(samples, 44100), std::runtime_error);

  // The cached client follows a runtime remote_url change instead of being
  // constructed per call (F4-2 lesson).
  FakeSttServer secondServer;
  pointAt("http://127.0.0.1:" + std::to_string(secondServer.port()));
  CHECK(adapter.transcribe(samples, 16000) == "hola en");
  CHECK(secondServer.requests().at("POST /stt/v1/transcribe?lang=en") == 1);
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=en") == 1);

  // The body is binary s16 PCM: one int16 per sample.
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

  // The greeting leg runs TTS only: start() sets the language locally but
  // sends nothing on the STT wire.
  CHECK(server.requests().empty());

  const size_t chunksBefore = sink.of(true).size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The turn text came back over the wire and went out as the stt frame.
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
  // A bound-then-closed port: every connect is refused on the loopback.
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
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The existing error path: no stt frame, the stt_failed reaction frame
  // goes out instead, and the worker thread is still alive.
  CHECK_FALSE(sink.hasType("voice:stt"));
  argus::voice::v1::ServerFrame event;
  bool eventFound = false;
  for (const auto& frame : sink.snapshot())
    if (frame.has_event() && frame.event().because() == "stt_failed") {
      event = frame;
      eventFound = true;
    }
  CHECK(eventFound);

  // The session still serves another turn after the failure.
  sess->history.clear();
  VoiceSessionTestAccess::runTurn(session, *sess, samples);
  CHECK(sink.size() > framesBefore);

  session.stop(sink);
  pointAt("");
}
