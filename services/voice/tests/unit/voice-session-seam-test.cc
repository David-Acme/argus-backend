#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <test-support/fake-voice-sink.hxx>

#include <config/config-service.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct FakeStt final : IVoiceStt
{
  std::string transcript{"hola argus"};
  std::atomic<int> transcribeCalls{0};
  std::mutex mutex;
  std::vector<std::string> languages;

  std::string transcribe(const VoiceTranscribeInput& input) override
  {
    ++transcribeCalls;
    std::scoped_lock lock(mutex);
    languages.push_back(input.language);
    return transcript;
  }
};

struct FakeTts final : IVoiceTts
{
  int synthesizeCalls{0};
  std::string lastText;

  float defaultSpeed(std::stop_token = {}) const override { return 1.25F; }
  int sampleRate(std::stop_token = {}) const override { return 22050; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    ++synthesizeCalls;
    lastText = input.request.text;
    input.onChunk(std::vector<float>(512, 0.25F));
  }
};

struct BlockingTts final : IVoiceTts
{
  std::atomic<bool> entered{false};
  std::atomic<bool> cancelled{false};

  float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::unique_lock lock(mutex);
    entered.store(true);
    ready.wait_for(lock, input.cancellation, std::chrono::seconds(3), [] { return false; });
    cancelled.store(input.cancellation.stop_requested());
  }
};

struct ConfigBlockingTts final : IVoiceTts
{
  mutable std::atomic<bool> entered{false};
  mutable std::atomic<bool> cancelled{false};

  float defaultSpeed(std::stop_token cancellation = {}) const override
  {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::unique_lock lock(mutex);
    entered.store(true);
    ready.wait_for(lock, cancellation, std::chrono::seconds(3),
                   [] { return false; });
    cancelled.store(cancellation.stop_requested());
    if (cancellation.stop_requested())
      throw std::runtime_error("synthesis cancelled");
    return 1.0F;
  }
  int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    input.onChunk(std::vector<float>(512, 0.25F));
  }
};

struct FakeLlm final : IVoiceLlm
{
  int chatStreamCalls{0};
  size_t lastPromptMessages{0};
  int64_t lastUserId{0};
  UserRole lastRole{UserRole::Owner};
  std::string lastLang;

  void chatStream(LlmStreamInput input) override
  {
    ++chatStreamCalls;
    lastPromptMessages = input.request.messages.size();
    lastUserId = input.request.userId;
    lastRole = input.request.role;
    lastLang = input.request.lang;
    input.onToken("Hola de nuevo.", false);
    input.onToken("", true);
  }
};

struct FailingLlm final : IVoiceLlm
{
  void chatStream(LlmStreamInput) override
  {
    throw std::runtime_error("llm busy");
  }
};

struct BlockingLlm final : IVoiceLlm
{
  std::atomic<bool> entered{false};
  std::atomic<bool> cancelled{false};
  std::atomic<int> calls{0};

  void chatStream(LlmStreamInput input) override
  {
    if (calls.fetch_add(1) > 0) {
      input.onToken("Sigo aqui.", false);
      input.onToken("", true);
      return;
    }
    input.onToken("Empiezo", false);
    std::mutex mutex;
    std::condition_variable_any ready;
    std::unique_lock lock(mutex);
    entered.store(true);
    ready.wait_for(lock, input.cancellation, std::chrono::seconds(5),
                   [] { return false; });
    cancelled.store(input.cancellation.stop_requested());
    if (input.cancellation.stop_requested())
      throw std::runtime_error("argus-llm stream cancelled");
    input.onToken(" tarde.", false);
    input.onToken("", true);
  }
};


struct ScriptedVadModel final : VadModel
{
  explicit ScriptedVadModel(std::shared_ptr<std::atomic<int>> counter)
      : windows(std::move(counter))
  {
  }

  float probability(std::span<const float> window) override
  {
    ++*windows;
    return window.back();
  }

  void reset() override {}

  std::shared_ptr<std::atomic<int>> windows;
};

struct ScriptedVad final : IVoiceVad
{
  std::shared_ptr<std::atomic<int>> windows = std::make_shared<std::atomic<int>>(0);

  [[nodiscard]] std::unique_ptr<VadModel> createModel() const override
  {
    return std::make_unique<ScriptedVadModel>(windows);
  }
};

struct StreamingTts final : IVoiceTts
{
  std::atomic<int> calls{0};
  std::atomic<int> cancelled{0};

  [[nodiscard]] float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  [[nodiscard]] int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    ++calls;
    for (int i = 0; i < 500 && !input.cancellation.stop_requested(); ++i) {
      input.onChunk(std::vector<float>(320, 0.1F));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (input.cancellation.stop_requested())
      ++cancelled;
  }
};

struct RecordingStt final : IVoiceStt
{
  std::mutex mutex;
  std::vector<std::vector<float>> turns;

  std::string transcribe(const VoiceTranscribeInput& input) override
  {
    std::scoped_lock lock(mutex);
    turns.push_back(input.samples);
    return "espera argus";
  }

  size_t count()
  {
    std::scoped_lock lock(mutex);
    return turns.size();
  }
};

class DuplexConfig
{
public:
  explicit DuplexConfig(int guardMs)
  {
    std::ofstream file(kPath);
    file << "[vad]\n"
         << "threshold = 0.45\n"
         << "neg_threshold = 0.25\n"
         << "min_speech_frames = 5\n"
         << "min_silence_frames = 12\n"
         << "pre_roll_frames = 10\n"
         << "min_turn_ms = 240\n"
         << "min_mean_prob = 0.35\n"
         << "denoise = false\n"
         << "barge_threshold = 0.7\n"
         << "barge_min_frames = 8\n"
         << "barge_guard_ms = " << guardMs << "\n";
    file.close();
    ConfigService::load(kPath);
  }

  ~DuplexConfig()
  {
    {
      std::ofstream file(kPath);
    }
    ConfigService::load(kPath);
    std::remove(kPath);
  }

  DuplexConfig(const DuplexConfig&) = delete;
  DuplexConfig& operator=(const DuplexConfig&) = delete;

private:
  static constexpr const char* kPath = "voice-duplex-test-config.toml";
};

constexpr int kWindow = 512;

struct FeedInput
{
  VoiceSessionService& service;
  VoiceSessionSink& sink;
  float prob{0.0F};
  int windows{0};
};

void feed(const FeedInput& input)
{
  const auto value = static_cast<int16_t>(input.prob * 32767.0F);
  const auto low = static_cast<char>(static_cast<uint16_t>(value) & 0xFFU);
  const auto high = static_cast<char>((static_cast<uint16_t>(value) >> 8U) & 0xFFU);
  std::string pcm;
  pcm.reserve(static_cast<size_t>(input.windows) * kWindow * 2);
  for (int i = 0; i < input.windows * kWindow; ++i) {
    pcm.push_back(low);
    pcm.push_back(high);
  }
  input.service.feedPcm(input.sink, {.data = pcm.data(), .size = pcm.size()});
}

argus::voice::v1::VoiceStart duplexStart()
{
  argus::voice::v1::VoiceStart start;
  start.mutable_identity()->set_user_id(7);
  start.mutable_identity()->set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  start.mutable_identity()->set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  start.mutable_identity()->set_name("Ana");
  start.set_mode(argus::voice::v1::VOICE_MODE_DUPLEX);
  return start;
}

std::vector<int64_t> interruptedIds(const FakeVoiceSink& sink)
{
  std::vector<int64_t> ids;
  for (const auto& frame : sink.snapshot())
    if (frame.has_interrupted())
      ids.push_back(frame.interrupted().id());
  return ids;
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

  static const IVoiceStt& sttOf(VoiceSessionService& service)
  {
    return service.stt_;
  }

  static const IVoiceTts& ttsOf(VoiceSessionService& service)
  {
    return service.tts_;
  }

  static const IVoiceLlm& llmOf(VoiceSessionService& service)
  {
    return service.llm_;
  }

  static const IVoiceIdentity& identityOf(VoiceSessionService& service)
  {
    return service.identity_;
  }
};

TEST_CASE("VoiceSessionService default-constructs on the remote seam")
{
  VoiceSessionService session;
  CHECK(&VoiceSessionTestAccess::sttOf(session) == &voiceStt());
  CHECK(&VoiceSessionTestAccess::ttsOf(session) == &voiceTts());
  CHECK(&VoiceSessionTestAccess::llmOf(session) == &voiceLlm());
  CHECK(&VoiceSessionTestAccess::identityOf(session) == &voiceIdentity());
  CHECK(dynamic_cast<RemoteVoiceStt*>(&voiceStt()) != nullptr);
  CHECK(dynamic_cast<RemoteVoiceTts*>(&voiceTts()) != nullptr);
  CHECK(dynamic_cast<RemoteVoiceLlm*>(&voiceLlm()) != nullptr);
  CHECK(dynamic_cast<GrpcVoiceIdentity*>(&voiceIdentity()) != nullptr);
}

TEST_CASE("Voice session start speaks the greeting through the injected seam")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  session.start(sink, voiceIdentity);

  CHECK(waitFor([&] {
    return sink.hasType("voice:assistant");
  }));
  CHECK(tts.synthesizeCalls > 0);
  CHECK(tts.lastText.find("Argus") != std::string::npos);
  CHECK_FALSE(sink.of(true).empty());
  for (const auto& frame : sink.snapshot())
    if (frame.has_assistant())
      CHECK(frame.assistant().text().find("Argus") != std::string::npos);
  CHECK_FALSE(sink.hasType("voice:turn"));
  for (const auto& frame : sink.snapshot())
    if (frame.has_assistant())
      CHECK(frame.assistant().turn_id() == 0);

  session.stop(sink);
  CHECK(sink.hasType("voice:done"));
}

TEST_CASE("A turn runs STT, LLM and TTS against the injected fakes")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_RESIDENT);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  session.start(sink, voiceIdentity);
  CHECK(waitFor([&] {
    auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
    return tts.synthesizeCalls > 0 && !sess->speaking.load();
  }));

  const size_t chunksBefore = sink.of(true).size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK(stt.transcribeCalls == 1);
  argus::voice::v1::ServerFrame sttFrame;
  for (const auto& frame : sink.snapshot())
    if (frame.has_stt())
      sttFrame = frame;
  CHECK(sttFrame.stt().text() == "hola argus");
  CHECK(sttFrame.stt().final());

  CHECK(llm.chatStreamCalls == 1);
  CHECK(llm.lastPromptMessages == 3);
  CHECK(llm.lastUserId == 7);
  CHECK(llm.lastRole == UserRole::Resident);
  CHECK(llm.lastLang == "es");
  {
    std::scoped_lock lock(stt.mutex);
    CHECK(stt.languages == std::vector<std::string>{"es"});
  }

  CHECK(tts.synthesizeCalls == 2);
  CHECK(tts.lastText == "Hola de nuevo.");
  CHECK(sink.of(true).size() > chunksBefore);

  bool assistantFound = false;
  for (const auto& frame : sink.snapshot())
    if (frame.has_assistant() && frame.assistant().text() == "Hola de nuevo.")
      assistantFound = true;
  CHECK(assistantFound);

  CHECK(sess->history.size() == 4);
  CHECK(sess->history[3].content == "Hola de nuevo.");

  session.stop(sink);
}

TEST_CASE("Skip and stop cancel blocked voice synthesis")
{
  BlockingTts tts;
  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});
  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  service.start(sink, voiceIdentity);
  CHECK(waitFor([&] { return tts.entered.load(); }, 1000));

  SUBCASE("skip cancels before another chunk arrives")
  {
    service.skip(sink);
    CHECK(waitFor([&] { return tts.cancelled.load(); }, 1000));
    service.stop(sink);
  }
  SUBCASE("disconnect and stop cancel before joining the worker")
  {
    sink.close();
    const auto start = std::chrono::steady_clock::now();
    service.stop(sink);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    CHECK(tts.cancelled.load());
  }
  CHECK(sink.of(true).empty());
}

TEST_CASE("Skip cancels a blocked voice config fetch")
{
  ConfigBlockingTts tts;
  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});
  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  service.start(sink, voiceIdentity);
  CHECK(waitFor([&] { return tts.entered.load(); }, 1000));
  service.skip(sink);
  CHECK(waitFor([&] { return tts.cancelled.load(); }, 1000));
  service.stop(sink);
}

TEST_CASE("The spoken name is written once through the identity seam")
{
  FakeStt stt;
  stt.transcript = "me llamo Juan";
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()};
  VoiceSessionService session(seam);

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  session.start(sink, voiceIdentity);
  CHECK(waitFor([&] {
    auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
    return tts.synthesizeCalls > 0 && !sess->speaking.load();
  }));

  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  REQUIRE(identity.writes.size() == 1);
  CHECK(identity.writes[0].userId == 7);
  CHECK(identity.writes[0].name == "Juan");
  CHECK(identity.writes[0].role == "owner");

  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});
  CHECK(identity.writes.size() == 1);

  session.stop(sink);
}

TEST_CASE("A failed answer rolls the user turn back and says so")
{
  FakeStt stt;
  FakeTts tts;
  FailingLlm llm;
  FakeIdentity identity;
  VoiceSessionService session({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_RESIDENT);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  session.start(sink, voiceIdentity);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  CHECK(waitFor([&] { return tts.synthesizeCalls > 0 && !sess->speaking.load(); }));

  const std::vector<float> samples(1600, 0.1F);
  VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});

  CHECK(sess->history.size() == 2);
  CHECK(sess->history.back().role == "assistant");
  CHECK(tts.synthesizeCalls == 2);
  CHECK(tts.lastText == "Perdona, ahora mismo no he podido responder.");

  session.stop(sink);
}

TEST_CASE("The history trims in whole turns and keeps the system prompt")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceSessionService session({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_RESIDENT);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  session.start(sink, voiceIdentity);
  auto sess = VoiceSessionTestAccess::sessionOf(session, sink);
  CHECK(waitFor([&] { return tts.synthesizeCalls > 0 && !sess->speaking.load(); }));

  const std::vector<float> samples(1600, 0.1F);
  for (int turn = 0; turn < 9; ++turn)
    VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});
  CHECK(sess->history.size() == 20);

  for (int turn = 0; turn < 3; ++turn)
    VoiceSessionTestAccess::runTurn({.service = session, .session = *sess, .samples = samples});
  CHECK(sess->history.size() == 15);
  CHECK(sess->history[0].role == "system");
  CHECK(sess->history[1].role == "user");
  CHECK(sess->history.back().role == "assistant");

  session.stop(sink);
}

TEST_CASE("A second start on a live session keeps the first one")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceSessionService session({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_RESIDENT);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  session.start(sink, voiceIdentity);
  const auto first = VoiceSessionTestAccess::sessionOf(session, sink);

  session.start(sink, voiceIdentity);
  CHECK(VoiceSessionTestAccess::sessionOf(session, sink) == first);

  session.stop(sink);
}

TEST_CASE("Skip cancels the LLM generation and the next turn does not wait for it")
{
  FakeStt stt;
  FakeTts tts;
  BlockingLlm llm;
  FakeIdentity identity;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});

  FakeVoiceSink sink;
  argus::voice::v1::VoiceIdentity voiceIdentity;
  voiceIdentity.set_user_id(7);
  voiceIdentity.set_role(argus::voice::v1::VOICE_ROLE_OWNER);
  voiceIdentity.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  voiceIdentity.set_name("Ana");
  service.start(sink, voiceIdentity);
  auto sess = VoiceSessionTestAccess::sessionOf(service, sink);
  CHECK(waitFor([&] { return tts.synthesizeCalls > 0 && !sess->speaking.load(); }));

  const std::vector<float> samples(1600, 0.1F);
  const auto started = std::chrono::steady_clock::now();
  std::thread turn([&] {
    VoiceSessionTestAccess::runTurn({.service = service, .session = *sess, .samples = samples});
  });
  CHECK(waitFor([&] { return llm.entered.load(); }, 1000));
  service.skip(sink);
  turn.join();
  CHECK(llm.cancelled.load());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
  const int spokenAfterSkip = tts.synthesizeCalls;

  VoiceSessionTestAccess::runTurn({.service = service, .session = *sess, .samples = samples});
  CHECK(llm.calls.load() == 2);
  CHECK(tts.synthesizeCalls == spokenAfterSkip + 1);
  CHECK(tts.lastText == "Sigo aqui.");

  service.stop(sink);
}

TEST_CASE("Concurrent sessions transcribe in their own language")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  FakeIdentity identity;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = voiceVad()});

  FakeVoiceSink spanishSink;
  argus::voice::v1::VoiceIdentity spanish;
  spanish.set_user_id(7);
  spanish.set_role(argus::voice::v1::VOICE_ROLE_RESIDENT);
  spanish.set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
  spanish.set_name("Ana");
  service.start(spanishSink, spanish);

  FakeVoiceSink englishSink;
  argus::voice::v1::VoiceIdentity english;
  english.set_user_id(8);
  english.set_role(argus::voice::v1::VOICE_ROLE_GUEST);
  english.set_language(argus::voice::v1::VOICE_LANGUAGE_EN);
  english.set_name("Bob");
  service.start(englishSink, english);

  auto es = VoiceSessionTestAccess::sessionOf(service, spanishSink);
  auto en = VoiceSessionTestAccess::sessionOf(service, englishSink);
  CHECK(waitFor([&] {
    return spanishSink.hasType("voice:assistant") &&
           englishSink.hasType("voice:assistant") && !es->speaking.load() &&
           !en->speaking.load();
  }));

  const std::vector<float> samples(1600, 0.1F);
  VoiceSessionTestAccess::runTurn({.service = service, .session = *es, .samples = samples});
  CHECK(llm.lastLang == "es");
  CHECK(llm.lastRole == UserRole::Resident);
  VoiceSessionTestAccess::runTurn({.service = service, .session = *en, .samples = samples});
  CHECK(llm.lastLang == "en");
  CHECK(llm.lastRole == UserRole::Guest);
  VoiceSessionTestAccess::runTurn({.service = service, .session = *es, .samples = samples});

  {
    std::scoped_lock lock(stt.mutex);
    CHECK(stt.languages == std::vector<std::string>{"es", "en", "es"});
  }

  service.stop(spanishSink);
  service.stop(englishSink);
}

TEST_CASE("A duplex session keeps feeding the VAD while the assistant speaks")
{
  DuplexConfig config(300);
  BlockingTts tts;
  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  service.start(sink, duplexStart());
  auto sess = VoiceSessionTestAccess::sessionOf(service, sink);
  CHECK(waitFor([&] { return tts.entered.load() && sess->speaking.load(); }, 1000));

  feed({.service = service, .sink = sink, .prob = 0.0F, .windows = 4});
  CHECK(waitFor([&] { return vad.windows->load() >= 4; }, 1000));
  CHECK(sess->speaking.load());

  service.stop(sink);
}

TEST_CASE("A half-duplex session still drops PCM while the assistant speaks")
{
  DuplexConfig config(300);
  BlockingTts tts;
  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  argus::voice::v1::VoiceStart start = duplexStart();
  start.set_mode(argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  service.start(sink, start);
  auto sess = VoiceSessionTestAccess::sessionOf(service, sink);
  CHECK(waitFor([&] { return tts.entered.load() && sess->speaking.load(); }, 1000));

  feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 4});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(vad.windows->load() == 0);
  {
    std::scoped_lock lock(sess->pcmMutex);
    CHECK(sess->pcmQueue.empty());
  }

  service.stop(sink);
  CHECK_FALSE(sink.hasType("voice:turn"));
  CHECK_FALSE(sink.hasType("voice:interrupted"));
}

TEST_CASE("Sustained speech over the assistant interrupts its turn exactly once")
{
  DuplexConfig config(100);
  StreamingTts tts;
  RecordingStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  service.start(sink, duplexStart());
  CHECK(waitFor([&] { return sink.hasType("voice:turn"); }, 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 12});
  CHECK(waitFor([&] { return sink.hasType("voice:interrupted"); }, 2000));
  CHECK(waitFor([&] { return tts.cancelled.load() == 1; }, 2000));
  CHECK(interruptedIds(sink) == std::vector<int64_t>{1});
  CHECK(llm.chatStreamCalls == 0);

  const size_t framesAtInterrupt = sink.size();
  feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 20});
  feed({.service = service, .sink = sink, .prob = 0.0F, .windows = 14});
  CHECK(waitFor([&] { return stt.count() == 1; }, 3000));
  {
    std::scoped_lock lock(stt.mutex);
    REQUIRE(stt.turns.size() == 1);
    CHECK(stt.turns[0].size() == static_cast<size_t>(44 * kWindow));
    CHECK(stt.turns[0].front() > 0.9F);
  }

  CHECK(waitFor([&] {
    for (const auto& frame : sink.snapshot())
      if (frame.has_turn() && frame.turn().id() == 2)
        return true;
    return false;
  }, 2000));
  CHECK(interruptedIds(sink) == std::vector<int64_t>{1});
  const auto frames = sink.snapshot();
  bool interruptedTurnSpoke = false;
  for (size_t i = framesAtInterrupt; i < frames.size(); ++i)
    interruptedTurnSpoke = interruptedTurnSpoke ||
                           (frames[i].has_assistant() &&
                            frames[i].assistant().turn_id() == 1) ||
                           (frames[i].has_turn() && frames[i].turn().id() == 1);
  CHECK_FALSE(interruptedTurnSpoke);

  service.stop(sink);
}

TEST_CASE("Short or weak speech over the assistant does not interrupt it")
{
  DuplexConfig config(100);
  StreamingTts tts;
  RecordingStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  service.start(sink, duplexStart());
  CHECK(waitFor([&] { return sink.hasType("voice:turn"); }, 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  for (int blip = 0; blip < 4; ++blip) {
    feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 7});
    feed({.service = service, .sink = sink, .prob = 0.0F, .windows = 2});
  }
  feed({.service = service, .sink = sink, .prob = 0.6F, .windows = 30});
  CHECK(waitFor([&] { return vad.windows->load() >= 66; }, 2000));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK_FALSE(sink.hasType("voice:interrupted"));
  CHECK(tts.cancelled.load() == 0);
  CHECK(stt.count() == 0);

  service.stop(sink);
}

TEST_CASE("Nothing interrupts the assistant before the barge-in guard elapses")
{
  DuplexConfig config(60000);
  StreamingTts tts;
  RecordingStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  service.start(sink, duplexStart());
  CHECK(waitFor([&] { return sink.hasType("voice:turn"); }, 1000));

  feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 24});
  CHECK(waitFor([&] { return vad.windows->load() >= 24; }, 2000));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK_FALSE(sink.hasType("voice:interrupted"));
  CHECK(tts.cancelled.load() == 0);
  CHECK(stt.count() == 0);

  service.stop(sink);
}

TEST_CASE("voice:turn precedes the first audio of every duplex turn")
{
  DuplexConfig config(300);
  FakeTts tts;
  FakeStt stt;
  FakeLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService service({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  FakeVoiceSink sink;
  service.start(sink, duplexStart());
  CHECK(waitFor([&] { return sink.hasType("voice:assistant"); }, 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  feed({.service = service, .sink = sink, .prob = 0.95F, .windows = 20});
  feed({.service = service, .sink = sink, .prob = 0.0F, .windows = 14});
  CHECK(waitFor([&] {
    int assistants = 0;
    for (const auto& frame : sink.snapshot())
      if (frame.has_assistant())
        ++assistants;
    return assistants == 2;
  }, 3000));

  std::vector<int64_t> turns;
  std::vector<int64_t> assistantTurns;
  bool audioAnnounced = false;
  bool everyChunkAnnounced = true;
  for (const auto& frame : sink.snapshot()) {
    if (frame.has_turn()) {
      turns.push_back(frame.turn().id());
      audioAnnounced = true;
    }
    else if (frame.has_tts_chunk()) {
      everyChunkAnnounced = everyChunkAnnounced && audioAnnounced;
    }
    else if (frame.has_assistant()) {
      assistantTurns.push_back(frame.assistant().turn_id());
      audioAnnounced = false;
    }
  }
  CHECK(turns == std::vector<int64_t>{1, 2});
  CHECK(assistantTurns == std::vector<int64_t>{1, 2});
  CHECK(everyChunkAnnounced);
  CHECK_FALSE(sink.hasType("voice:interrupted"));

  service.stop(sink);
}
