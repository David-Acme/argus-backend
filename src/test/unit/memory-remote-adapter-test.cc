#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/memory-tool-descriptors.hxx>
#include <shared/services/memory/remote/remote-memory-service-adapter.hxx>
#include <shared/services/tools/tool-registry.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/reader.h>
#include <json/value.h>
#include <atomic>
#include <string>
#include <thread>

namespace
{

// The fake argus-memory wire: one frozen envelope per path, with the last
// request recorded so the forwarding contract is asserted end to end.
class FakeMemoryServer
{
public:
  FakeMemoryServer()
  {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) ==
            0);
    port_ = ntohs(addr.sin_port);
    REQUIRE(::listen(fd_, 8) == 0);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeMemoryServer()
  {
    stop_ = true;
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    thread_.join();
  }

  int port() const { return port_; }
  const std::string& lastPath() const { return lastPath_; }
  const Json::Value& lastBody() const { return lastBody_; }

private:
  void serve()
  {
    while (!stop_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0)
        break;
      handle(client);
      ::close(client);
    }
  }

  void handle(int client)
  {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        return;
      request.append(buffer, static_cast<size_t>(n));
    }

    const auto lineEnd = request.find("\r\n");
    const auto pathStart = request.find(' ') + 1;
    lastPath_ = request.substr(pathStart, lineEnd - pathStart - 9);
    const auto clAt = request.find("Content-Length: ");
    size_t contentLength = 0;
    if (clAt != std::string::npos) {
      contentLength = static_cast<size_t>(
          std::stoul(request.substr(clAt + 16)));
    }
    std::string payload = request.substr(request.find("\r\n\r\n") + 4);
    while (payload.size() < contentLength) {
      const auto n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        break;
      payload.append(buffer, static_cast<size_t>(n));
    }
    Json::Reader reader;
    reader.parse(payload, lastBody_);

    std::string info;
    if (lastPath_ == "/memory/v1/remember")
      info = R"({"ok":true,"output":"anotado","data":{"fact_id":77}})";
    else if (lastPath_ == "/memory/v1/recall")
      info = R"({"ok":true,"output":"mi hermana se llama Ana","data":{}})";
    else if (lastPath_ == "/memory/v1/forget")
      info = R"({"ok":true,"output":"olvidado","data":{}})";
    else if (lastPath_ == "/memory/v1/procedure-run")
      info =
          R"({"ok":false,"output":"no hay un procedimiento conocido","data":{}})";
    else if (lastPath_ == "/memory/v1/capture")
      info = lastBody_["text"].asString().find("quizas") ==
                     std::string::npos
                 ? R"({"outcome":"stored","fact_id":88})"
                 : R"({"outcome":"deferred","fact_id":0})";
    else if (lastPath_ == "/memory/v1/compact")
      info = R"({"queued":true})";
    else if (lastPath_ == "/memory/v1/durable-transcript")
      info = R"({"text":"user: durable"})";
    else
      info = R"({})";

    const std::string body =
        R"({"status":200,"info":)" + info + R"(,"errors":{}})";
    std::string reply = "HTTP/1.1 200 OK\r\n";
    reply += "Content-Type: application/json\r\n";
    reply += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    reply += "Connection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < reply.size()) {
      const auto n =
          ::send(client, reply.data() + sent, reply.size() - sent, 0);
      if (n <= 0)
        break;
      sent += static_cast<size_t>(n);
    }
  }

  int fd_{-1};
  int port_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::string lastPath_;
  Json::Value lastBody_;
};

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("memory.remote_url", url);
}

constexpr const char* kScratchConfig = "memory-remote-adapter-test.toml";

tools::ToolCall memoryCall(const std::string& name, const std::string& argsJson)
{
  tools::ToolCall call;
  call.name = name;
  Json::Reader reader;
  REQUIRE(reader.parse(argsJson, call.arguments));
  call.context.userId = 9;
  call.context.lang = "es";
  call.context.sessionId = "sess-1";
  return call;
}

} // namespace

TEST_CASE("without memory.remote_url the remote adapter never initializes and "
          "never registers the memory tools")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[memory]\nremote_url = \"\"\n";
  }
  ConfigService::load(kScratchConfig);

  RemoteMemoryServiceAdapter adapter;
  CHECK_FALSE(adapter.initialize());
  CHECK_FALSE(adapter.isLoaded());
  CHECK_FALSE(adapter.health()["loaded"].asBool());
  CHECK(ToolRegistry::instance().find("memory.remember") == nullptr);
  std::remove(kScratchConfig);
}

TEST_CASE("the remote adapter registers the shared descriptors and forwards "
          "the tool surface to argus-memory")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[memory]\nremote_url = \"placeholder\"\n";
  }
  ConfigService::load(kScratchConfig);

  const FakeMemoryServer server;
  pointAt("127.0.0.1:" + std::to_string(server.port()));

  RemoteMemoryServiceAdapter adapter;
  REQUIRE(adapter.initialize());
  CHECK(adapter.isLoaded());
  CHECK(adapter.health()["remote"].asString() ==
        "127.0.0.1:" + std::to_string(server.port()));

  // The tool loop cannot tell the substrates apart: the registered
  // descriptors are byte-for-byte the shared metadata.
  auto& registry = ToolRegistry::instance();
  const std::vector<tools::ToolDescriptor> shared = memoryToolDescriptors();
  for (const auto& descriptor : shared) {
    const tools::ToolDescriptor* registered = registry.find(descriptor.name);
    REQUIRE(registered != nullptr);
    CHECK(registered->description == descriptor.description);
    REQUIRE(registered->arguments.size() == descriptor.arguments.size());
    for (size_t i = 0; i < descriptor.arguments.size(); ++i) {
      CHECK(registered->arguments[i].name == descriptor.arguments[i].name);
      CHECK(registered->arguments[i].type == descriptor.arguments[i].type);
      CHECK(registered->arguments[i].required ==
            descriptor.arguments[i].required);
    }
  }

  // ── memory.remember forwards arguments plus the derived context ────────
  const tools::ToolResult remembered =
      registry.find("memory.remember")->handler(memoryCall(
          "memory.remember",
          R"({"subject":"mi hermana","predicate":"se llama","value":"Ana",
              "type":"persona"})"));
  CHECK(remembered.tool == "memory.remember");
  CHECK(remembered.ok);
  CHECK(remembered.output == "anotado");
  CHECK(remembered.data["fact_id"].asInt64() == 77);
  CHECK(server.lastPath() == "/memory/v1/remember");
  CHECK(server.lastBody()["context"]["user_id"].asInt64() == 9);
  CHECK(server.lastBody()["context"]["lang"] == "es");
  CHECK(server.lastBody()["context"]["session_id"] == "sess-1");
  CHECK(server.lastBody()["subject"] == "mi hermana");

  // ── memory.recall maps the info block into the ToolResult ──────────────
  const tools::ToolResult recalled = registry.find("memory.recall")->handler(
      memoryCall("memory.recall", R"({"query":"mi hermana"})"));
  CHECK(recalled.ok);
  CHECK(recalled.output == "mi hermana se llama Ana");
  CHECK(server.lastPath() == "/memory/v1/recall");

  // ── memory.procedure.run is served on the wire but is not a model-facing
  // tool: only the three shared descriptors are registered ────────────────
  CHECK(registry.find("memory.procedure.run") == nullptr);

  // ── memory.forget ──────────────────────────────────────────────────────
  const tools::ToolResult forgotten = registry.find("memory.forget")->handler(
      memoryCall("memory.forget", R"({"fact_id":77})"));
  CHECK(forgotten.ok);
  CHECK(server.lastPath() == "/memory/v1/forget");
  CHECK(server.lastBody()["fact_id"].asInt64() == 77);

  // ── capture: stored and deferred outcomes map onto CaptureOutcome ──────
  const CaptureResult stored = adapter.captureExplicit(
      {.userId = 9, .lang = "es", .text = "mi hermana se llama Ana"});
  CHECK(stored.outcome == CaptureOutcome::Stored);
  CHECK(stored.factId == 88);
  CHECK(server.lastPath() == "/memory/v1/capture");

  const CaptureResult deferred = adapter.captureExplicit(
      {.userId = 9, .lang = "es", .text = "quizas me llamo Ana"});
  CHECK(deferred.outcome == CaptureOutcome::Deferred);
  CHECK(deferred.factId == 0);

  // ── compaction is enqueued and never inline ────────────────────────────
  adapter.enqueueCompaction(9, "user: hola\nassistant: hola", "es");
  CHECK(server.lastPath() == "/memory/v1/compact");
  CHECK(server.lastBody()["queued"].isNull());
  CHECK(server.lastBody()["transcript"].asString().find("hola") !=
        std::string::npos);

  // ── the durable transcript rides the wire ──────────────────────────────
  CHECK(adapter.durableTranscript("user: durable", "es") == "user: durable");
  CHECK(server.lastPath() == "/memory/v1/durable-transcript");

  adapter.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("with argus-memory down the registered tool handlers degrade "
          "instead of throwing")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[memory]\nremote_timeout_ms = 200\n";
  }
  ConfigService::load(kScratchConfig);

  // The adapter registers against a live wire, then the wire goes away —
  // the outage the tool loop must survive (substrate parity: the in-process
  // handlers return ok=false, they never throw).
  std::string url;
  {
    const FakeMemoryServer server;
    url = "127.0.0.1:" + std::to_string(server.port());
    pointAt(url);
    RemoteMemoryServiceAdapter adapter;
    REQUIRE(adapter.initialize());
    CHECK(ToolRegistry::instance().find("memory.recall") != nullptr);
  }
  pointAt(url);

  RemoteMemoryServiceAdapter adapter;
  REQUIRE(adapter.initialize());

  auto& registry = ToolRegistry::instance();
  for (const char* name :
       {"memory.remember", "memory.recall", "memory.forget",
        "procedure.run"}) {
    INFO("handler: ", name);
    const tools::ToolDescriptor* descriptor = registry.find(name);
    REQUIRE(descriptor != nullptr);
    tools::ToolResult result;
    CHECK_NOTHROW(result = descriptor->handler(
                      memoryCall(name, R"({"query":"mi hermana"})")));
    CHECK_FALSE(result.ok);
    CHECK(result.tool == name);
    CHECK(result.output == "la memoria no esta disponible ahora");
  }

  adapter.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("with argus-memory down the adapter degrades instead of crashing")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[memory]\nremote_url = \"127.0.0.1:1\"\n"
           << "remote_timeout_ms = 200\n";
  }
  ConfigService::load(kScratchConfig);

  RemoteMemoryServiceAdapter adapter;
  // The gate only decides the substrate: an unreachable argus-memory still
  // disables the in-process stack.
  CHECK(adapter.initialize());
  CHECK(adapter.isLoaded());

  const CaptureResult capture = adapter.captureExplicit(
      {.userId = 9, .lang = "es", .text = "mi hermana se llama Ana"});
  CHECK(capture.outcome == CaptureOutcome::Rejected);
  CHECK(capture.factId == 0);

  CHECK_NOTHROW(
      adapter.enqueueCompaction(9, "user: hola\nassistant: hola", "es"));
  CHECK(adapter.durableTranscript("user: durable", "es").empty());

  adapter.shutdown();
  std::remove(kScratchConfig);
}
