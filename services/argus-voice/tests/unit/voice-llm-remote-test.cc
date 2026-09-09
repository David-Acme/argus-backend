#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-llm-server.hxx"
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

// In-process STT so the tests below isolate the LLM leg.
struct LocalFakeStt final : IVoiceStt
{
  std::string transcribe(const std::vector<float>&, int32_t) override
  {
    return "hola es";
  }

  bool setLanguage(const std::string&) override { return true; }
};

void pointLlmAt(const std::string& url)
{
  ConfigService::setRuntimeString("llm.remote_url", url);
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

namespace
{

ChatRequest greetingRequest()
{
  ChatRequest req;
  req.messages = {{"user", "Di hola"}};
  req.maxTokens = 16;
  return req;
}

std::vector<argus::voice::v1::ServerFrame>
assistantFrames(const FakeVoiceSink& sink)
{
  std::vector<argus::voice::v1::ServerFrame> out;
  for (const auto& frame : sink.snapshot())
    if (frame.has_assistant())
      out.push_back(frame);
  return out;
}

} // namespace

TEST_CASE("RemoteVoiceLlm serves the IVoiceLlm seam over the argus-llm wire")
{
  const std::vector<std::string> tokens = {"Hola", " de", " nuevo", ".",
                                           " Otra", " frase", "."};
  FakeLlmServer server({.tokens = tokens});
  pointLlmAt("http://127.0.0.1:" + std::to_string(server.port()));

  RemoteVoiceLlm adapter;

  std::vector<std::string> arrived;
  bool done = false;
  adapter.chatStream(greetingRequest(),
                     [&](const std::string& token, bool atEnd) {
                       if (atEnd)
                         done = true;
                       else
                         arrived.push_back(token);
                     });
  CHECK(done);
  CHECK(arrived == tokens);
  std::string joined;
  for (const auto& token : arrived)
    joined += token;
  CHECK(joined.find("done") == std::string::npos);

  LlmHttpClient client("http://127.0.0.1:" + std::to_string(server.port()),
                       120000);
  CHECK(client.chat(greetingRequest()) == joined);

  LlmPrefillStats stats;
  LlmStreamInput input;
  input.request = greetingRequest();
  input.stats = &stats;
  bool doneStats = false;
  input.onToken = [&](const std::string&, bool atEnd) {
    if (atEnd)
      doneStats = true;
  };
  client.chatStream(input);
  CHECK(doneStats);
  CHECK(stats.promptTokens == 7);
  CHECK(stats.decodedTokens == 7);

  FakeLlmServer coalesced({.tokens = tokens, .coalesce = true});
  pointLlmAt("http://127.0.0.1:" + std::to_string(coalesced.port()));
  std::vector<std::string> coalescedArrived;
  bool coalescedDone = false;
  adapter.chatStream(greetingRequest(),
                     [&](const std::string& token, bool atEnd) {
                       if (atEnd)
                         coalescedDone = true;
                       else
                         coalescedArrived.push_back(token);
                     });
  CHECK(coalescedDone);
  std::string coalescedText;
  for (const auto& token : coalescedArrived)
    coalescedText += token;
  CHECK(coalescedText == joined);

  FakeLlmServer second({.tokens = {"Adios"}});
  pointLlmAt("http://127.0.0.1:" + std::to_string(second.port()));
  std::string secondText;
  adapter.chatStream(greetingRequest(),
                     [&](const std::string& token, bool atEnd) {
                       if (!atEnd)
                         secondText += token;
                     });
  CHECK(secondText == "Adios");
  CHECK(second.requests().at("POST /llm/v1/chat-stream") == 1);
  CHECK(coalesced.requests().at("POST /llm/v1/chat-stream") == 1);

  FakeLlmServer down({.tokens = tokens, .status = 503});
  pointLlmAt("http://127.0.0.1:" + std::to_string(down.port()));
  CHECK_THROWS_AS(adapter.chatStream(greetingRequest(),
                                     [](const std::string&, bool) {}),
                  std::runtime_error);

  FakeLlmServer truncated({.tokens = tokens, .truncated = true});
  pointLlmAt("http://127.0.0.1:" + std::to_string(truncated.port()));
  CHECK_THROWS_AS(adapter.chatStream(greetingRequest(),
                                     [](const std::string&, bool) {}),
                  std::runtime_error);

  pointLlmAt("");
  CHECK_THROWS_AS(adapter.chatStream(greetingRequest(),
                                     [](const std::string&, bool) {}),
                  std::runtime_error);
}

TEST_CASE("The voice session speaks through the remote adapter")
{
  FakeSttServer sttServer;
  ConfigService::setRuntimeString("stt.remote_url",
                                  "http://127.0.0.1:" +
                                      std::to_string(sttServer.port()));
  FakeLlmServer llmServer({.tokens = {"Hola", " de", " nuevo", ".",
                                      " Otra", " frase", "."}});
  pointLlmAt("http://127.0.0.1:" + std::to_string(llmServer.port()));

  FakeTts tts;
  FakeIdentity identity;
  RemoteVoiceStt stt;
  RemoteVoiceLlm llm;
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

  const size_t chunksBefore = sink.of(true).size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK(sttServer.requests().at("POST /stt/v1/transcribe?lang=es") == 1);
  CHECK(llmServer.requests().at("POST /llm/v1/chat-stream") == 1);
  bool sttSeen = false;
  for (const auto& frame : sink.snapshot())
    if (frame.has_stt()) {
      CHECK(frame.stt().text() == "hola es");
      sttSeen = true;
    }
  CHECK(sttSeen);

  std::string spoken;
  const auto replies = assistantFrames(sink);
  REQUIRE(replies.size() >= 2);
  for (size_t i = 1; i < replies.size(); ++i)
    spoken += replies[i].assistant().text();
  CHECK(spoken == "Hola de nuevo. Otra frase.");
  CHECK(sink.of(true).size() > chunksBefore);

  session.stop(sink);
  pointLlmAt("");
  ConfigService::setRuntimeString("stt.remote_url", "");
}

TEST_CASE("An unreachable argus-llm degrades the turn, not the session")
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
  pointLlmAt("http://127.0.0.1:" + std::to_string(deadPort));

  FakeTts tts;
  LocalFakeStt stt;
  FakeIdentity identity;
  RemoteVoiceLlm llm;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity};

  FakeVoiceSink sink;
  VoiceSessionService session(seam);
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  session.start(sink, voiceIdentity);
  CHECK(waitFor([&] {
    return sink.hasType("voice:assistant");
  }));

  const size_t assistantBefore = assistantFrames(sink).size();
  const size_t chunksBefore = sink.of(true).size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK(assistantFrames(sink).size() == assistantBefore);
  CHECK(sink.of(true).size() == chunksBefore);

  FakeLlmServer llmServer({.tokens = {"Recuperado", "."}});
  pointLlmAt("http://127.0.0.1:" + std::to_string(llmServer.port()));
  sess->history.clear();
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});
  const auto replies = assistantFrames(sink);
  REQUIRE_FALSE(replies.empty());
  CHECK(replies.back().assistant().text() == "Recuperado.");

  session.stop(sink);
  pointLlmAt("");
}
