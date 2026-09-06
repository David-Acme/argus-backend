#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/socket/sync/services/voice-session-service.hxx>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

class FakeVoiceConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len,
            drogon::WebSocketMessageType) override
  {
    binaryFrames.emplace_back(msg, len);
  }

  void send(std::string_view msg, drogon::WebSocketMessageType) override
  {
    binaryFrames.emplace_back(msg);
  }

  void sendJson(const Json::Value& json, drogon::WebSocketMessageType) override
  {
    jsonFrames.push_back(json);
  }

  const trantor::InetAddress& localAddr() const override { return addr_; }
  const trantor::InetAddress& peerAddr() const override { return addr_; }
  bool connected() const override { return true; }
  bool disconnected() const override { return false; }
  void shutdown(drogon::CloseCode, const std::string&) override {}
  void forceClose() override {}
  void setPingMessage(const std::string&,
                      const std::chrono::duration<double>&) override
  {
  }
  void disablePing() override {}

  std::vector<std::string> binaryFrames;
  std::vector<Json::Value> jsonFrames;

private:
  trantor::InetAddress addr_;
};

struct FakeStt final : IVoiceStt
{
  std::string transcript{"hola argus"};
  int transcribeCalls{0};
  int setLanguageCalls{0};
  std::string lastLanguage;

  std::string transcribe(const std::vector<float>&, int32_t) override
  {
    ++transcribeCalls;
    return transcript;
  }

  bool setLanguage(const std::string& lang) override
  {
    ++setLanguageCalls;
    lastLanguage = lang;
    return true;
  }
};

struct FakeTts final : IVoiceTts
{
  int synthesizeCalls{0};
  std::string lastText;

  float defaultSpeed() const override { return 1.25F; }
  int sampleRate() const override { return 22050; }

  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) override
  {
    ++synthesizeCalls;
    lastText = req.text;
    onChunk(std::vector<float>(512, 0.25F));
  }
};

struct FakeLlm final : IVoiceLlm
{
  int chatStreamCalls{0};
  size_t lastPromptMessages{0};

  void chatStream(const ChatRequest& req, TokenCallback onToken) override
  {
    ++chatStreamCalls;
    lastPromptMessages = req.messages.size();
    onToken("Hola de nuevo.", false);
    onToken("", true);
  }
};

// Polls a condition on a worker thread for at most `ms` (the session runs
// its greeting on its own worker thread).
template <typename Pred>
bool waitFor(Pred ready, int ms = 5000)
{
  for (int waited = 0; waited < ms; waited += 20) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ready();
}

const Json::Value* frameOf(const FakeVoiceConnection& conn,
                           const std::string& type)
{
  const Json::Value* found = nullptr;
  for (const auto& frame : conn.jsonFrames)
    if (frame["type"] == type)
      found = &frame;
  return found;
}

} // namespace

// Must match the friend declaration inside VoiceSessionService (global scope).
struct VoiceSessionTestAccess
{
  static std::shared_ptr<VoiceSessionService::Session>
  sessionOf(VoiceSessionService& service,
            const drogon::WebSocketConnectionPtr& conn)
  {
    std::lock_guard<std::mutex> lock(service.mutex_);
    return service.sessions_.at(conn.get());
  }

  static void runTurn(VoiceSessionService& service,
                      VoiceSessionService::Session& session,
                      const std::vector<float>& samples)
  {
    service.processTurn(session, samples);
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
};

TEST_CASE("VoiceSessionService default-constructs on the legacy singleton seam")
{
  VoiceSessionService session;
  CHECK(&VoiceSessionTestAccess::sttOf(session) == &voiceStt());
  CHECK(&VoiceSessionTestAccess::ttsOf(session) == &voiceTts());
  CHECK(&VoiceSessionTestAccess::llmOf(session) == &voiceLlm());
  CHECK(dynamic_cast<SingletonVoiceStt*>(&voiceStt()) != nullptr);
  CHECK(dynamic_cast<SingletonVoiceTts*>(&voiceTts()) != nullptr);
  CHECK(dynamic_cast<SingletonVoiceLlm*>(&voiceLlm()) != nullptr);
}

TEST_CASE("Voice session start speaks the greeting through the injected seam")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "");

  CHECK(stt.setLanguageCalls == 1);
  CHECK(stt.lastLanguage == "es");

  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));
  CHECK(tts.synthesizeCalls > 0);
  CHECK(tts.lastText.find("Argus") != std::string::npos);
  CHECK_FALSE(conn->binaryFrames.empty());
  const Json::Value* assistant = frameOf(*conn, "voice:assistant");
  REQUIRE(assistant != nullptr);
  CHECK((*assistant)["payload"]["text"].asString().find("Argus") !=
        std::string::npos);

  session.stop(conn);
  CHECK(frameOf(*conn, "voice:done") != nullptr);
}

TEST_CASE("A turn runs STT, LLM and TTS against the injected fakes")
{
  FakeStt stt;
  FakeTts tts;
  FakeLlm llm;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "Ana");
  CHECK(waitFor([&] {
    auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
    return tts.synthesizeCalls > 0 && !sess->speaking.load();
  }));

  const size_t binaryBefore = conn->binaryFrames.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  CHECK(stt.transcribeCalls == 1);
  const Json::Value* sttFrame = frameOf(*conn, "voice:stt");
  REQUIRE(sttFrame != nullptr);
  CHECK((*sttFrame)["payload"]["text"] == "hola argus");
  CHECK((*sttFrame)["payload"]["final"] == true);

  CHECK(llm.chatStreamCalls == 1);
  CHECK(llm.lastPromptMessages == 3);

  CHECK(tts.synthesizeCalls == 2);
  CHECK(tts.lastText == "Hola de nuevo.");
  CHECK(conn->binaryFrames.size() > binaryBefore);

  const Json::Value* assistant = frameOf(*conn, "voice:assistant");
  REQUIRE(assistant != nullptr);
  CHECK((*assistant)["payload"]["text"] == "Hola de nuevo.");

  CHECK(sess->history.size() == 4);
  CHECK(sess->history[3].content == "Hola de nuevo.");

  session.stop(conn);
}
