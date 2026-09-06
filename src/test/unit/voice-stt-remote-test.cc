#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-stt-server.hxx"

#include <feature/socket/sync/services/voice-session-service.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <chrono>
#include <string>
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

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("stt.remote_url", url);
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

  // Ruling BE: the session start steers the language; the next transcribe
  // carries it as the lang parameter.
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
  RemoteVoiceStt stt;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "Ana");
  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));

  // The greeting leg runs TTS only: start() sets the language locally but
  // sends nothing on the STT wire.
  CHECK(server.requests().empty());

  const size_t binaryBefore = conn->binaryFrames.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The turn text came back over the wire and went out as voice:stt.
  CHECK(server.requests().at("POST /stt/v1/transcribe?lang=es") == 1);
  const Json::Value* sttFrame = frameOf(*conn, "voice:stt");
  REQUIRE(sttFrame != nullptr);
  CHECK((*sttFrame)["payload"]["text"] == "hola es");
  CHECK(conn->binaryFrames.size() > binaryBefore);

  session.stop(conn);
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
  RemoteVoiceStt stt;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "");
  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));

  const size_t framesBefore = conn->jsonFrames.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The existing error path: no voice:stt, the stt_failed reaction frame
  // goes out instead, and the worker thread is still alive.
  CHECK(frameOf(*conn, "voice:stt") == nullptr);
  const Json::Value* event = nullptr;
  for (const auto& frame : conn->jsonFrames)
    if (frame["type"] == "voice:event")
      event = &frame;
  REQUIRE(event != nullptr);
  CHECK((*event)["payload"]["because"] == "stt_failed");

  // The session still serves another turn after the failure.
  sess->history.clear();
  VoiceSessionTestAccess::runTurn(session, *sess, samples);
  CHECK(conn->jsonFrames.size() > framesBefore);

  session.stop(conn);
  pointAt("");
}
