#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-llm-server.hxx"
#include "fake-stt-server.hxx"

#include <feature/socket/sync/services/voice-engine-seam.hxx>
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

// In-process STT so the tests below isolate the LLM leg.
struct LocalFakeStt final : IVoiceStt
{
  std::string transcribe(const std::vector<float>&, int32_t) override
  {
    return "hola es";
  }

  bool setLanguage(const std::string&) override { return true; }
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

std::vector<const Json::Value*> framesOf(const FakeVoiceConnection& conn,
                                         const std::string& type)
{
  std::vector<const Json::Value*> found;
  for (const auto& frame : conn.jsonFrames)
    if (frame["type"] == type)
      found.push_back(&frame);
  return found;
}

void pointLlmAt(const std::string& url)
{
  ConfigService::setRuntimeString("llm.remote_url", url);
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

namespace
{

ChatRequest greetingRequest()
{
  ChatRequest req;
  req.messages = {{"user", "Di hola"}};
  req.maxTokens = 16;
  return req;
}

} // namespace

TEST_CASE("RemoteVoiceLlm serves the IVoiceLlm seam over the argus-llm wire")
{
  const std::vector<std::string> tokens = {"Hola", " de", " nuevo", ".",
                                           " Otra", " frase", "."};
  FakeLlmServer server({.tokens = tokens});
  pointLlmAt("http://127.0.0.1:" + std::to_string(server.port()));

  RemoteVoiceLlm adapter;

  // The stream leg: tokens arrive in wire order and the sentinel never
  // surfaces as a token.
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

  // The chat leg: the joined text comes back in the frozen envelope.
  LlmHttpClient client("http://127.0.0.1:" + std::to_string(server.port()),
                       120000);
  CHECK(client.chat(greetingRequest()) == joined);

  // The sentinel stats ride the last line when the caller asks for them.
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

  // Coalesced framing: tokens + sentinel in one chunk still splits cleanly.
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

  // The cached client follows a runtime remote_url change instead of being
  // constructed per call (F4-2 lesson).
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

  // A 503 envelope from argus-llm surfaces as an exception to the caller.
  FakeLlmServer down({.tokens = tokens, .status = 503});
  pointLlmAt("http://127.0.0.1:" + std::to_string(down.port()));
  CHECK_THROWS_AS(adapter.chatStream(greetingRequest(),
                                     [](const std::string&, bool) {}),
                  std::runtime_error);

  // A stream that ends without a sentinel is a protocol error.
  FakeLlmServer truncated({.tokens = tokens, .truncated = true});
  pointLlmAt("http://127.0.0.1:" + std::to_string(truncated.port()));
  CHECK_THROWS_AS(adapter.chatStream(greetingRequest(),
                                     [](const std::string&, bool) {}),
                  std::runtime_error);

  // No remote_url configured: the adapter refuses locally.
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
  RemoteVoiceStt stt;
  RemoteVoiceLlm llm;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};
  VoiceSessionService session(seam);

  auto conn = std::make_shared<FakeVoiceConnection>();
  session.start(conn, 0, VoiceLang::Es, "Ana");
  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));

  const size_t binaryBefore = conn->binaryFrames.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The transcribed text rode the STT wire and the streamed reply rode the
  // LLM wire, sentence by sentence as the tokens arrived.
  CHECK(sttServer.requests().at("POST /stt/v1/transcribe?lang=es") == 1);
  CHECK(llmServer.requests().at("POST /llm/v1/chat-stream") == 1);
  const Json::Value* sttFrame = frameOf(*conn, "voice:stt");
  REQUIRE(sttFrame != nullptr);
  CHECK((*sttFrame)["payload"]["text"] == "hola es");

  // The greeting is its own voice:assistant frame; only the turn's replies
  // go into the spoken text.
  std::string spoken;
  auto replies = framesOf(*conn, "voice:assistant");
  for (size_t i = 1; i < replies.size(); ++i)
    spoken += (*replies[i])["payload"]["text"].asString();
  CHECK(spoken == "Hola de nuevo. Otra frase.");
  CHECK(conn->binaryFrames.size() > binaryBefore);

  session.stop(conn);
  pointLlmAt("");
  ConfigService::setRuntimeString("stt.remote_url", "");
}

TEST_CASE("An unreachable argus-llm degrades the turn, not the session")
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
  pointLlmAt("http://127.0.0.1:" + std::to_string(deadPort));

  FakeTts tts;
  LocalFakeStt stt;
  RemoteVoiceLlm llm;
  VoiceEngineSeam seam{.stt = stt, .tts = tts, .llm = llm};

  auto conn = std::make_shared<FakeVoiceConnection>();
  VoiceSessionService session(seam);
  session.start(conn, 0, VoiceLang::Es, "");
  CHECK(waitFor([&] {
    return frameOf(*conn, "voice:assistant") != nullptr;
  }));

  const size_t assistantBefore =
      framesOf(*conn, "voice:assistant").size();
  const size_t binaryBefore = conn->binaryFrames.size();
  const std::vector<float> samples(1600, 0.1F);
  auto sess = VoiceSessionTestAccess::sessionOf(session, conn);
  VoiceSessionTestAccess::runTurn(session, *sess, samples);

  // The existing error path: the turn's LLM leg fails silently (logged), no
  // new voice:assistant goes out, and the worker thread is still alive.
  CHECK(framesOf(*conn, "voice:assistant").size() == assistantBefore);
  CHECK(conn->binaryFrames.size() == binaryBefore);

  // The cached client rebuilds on a remote_url change and the session still
  // serves another turn afterwards.
  FakeLlmServer llmServer({.tokens = {"Recuperado", "."}});
  pointLlmAt("http://127.0.0.1:" + std::to_string(llmServer.port()));
  sess->history.clear();
  VoiceSessionTestAccess::runTurn(session, *sess, samples);
  const Json::Value* assistant = frameOf(*conn, "voice:assistant");
  REQUIRE(assistant != nullptr);
  CHECK((*assistant)["payload"]["text"] == "Recuperado.");

  session.stop(conn);
  pointLlmAt("");
}
