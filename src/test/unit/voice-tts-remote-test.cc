#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-tts-server.hxx"

#include <feature/socket/sync/services/voice-session-service.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <chrono>
#include <cmath>
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
  RemoteVoiceTts tts;

  int chunks = 0;
  tts.synthesizeStream({.text = "Hola, soy Argus, tu asistente local.",
                        .lang = TtsLang::ES,
                        .voiceId = "M3",
                        .quality = TtsQuality::Auto,
                        .speed = 1.05F},
                       [&](const std::vector<float>&) { ++chunks; });
  CHECK(chunks == 2);

  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "");

  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));
  CHECK_FALSE(conn->binaryFrames.empty());

  const auto requests = server.requests();
  CHECK(requests.at("POST /tts/v1/synthesize-stream") >= 2);

  session.stop(conn);
  pointAt("");
}

TEST_CASE("An unreachable argus-tts degrades the speak leg, not the session")
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

  FakeStt stt;
  FakeLlm llm;
  RemoteVoiceTts tts;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "");

  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));
  CHECK(conn->binaryFrames.empty());

  session.stop(conn);
  pointAt("");
}
