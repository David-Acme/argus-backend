#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/voice-rpc-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <test-support/fake-voice-sink.hxx>
#include <test-support/voice-test-config.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <sys/mman.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kSyncSecret = "voice-reactor-lifetime-sync";
constexpr const char* kNotificationSecret = "voice-reactor-lifetime-notification";

struct SilentStt final : IVoiceStt
{
  std::string transcribe(const VoiceTranscribeInput&) override { return {}; }
};

struct PacedTts final : IVoiceTts
{
  static constexpr int kChunks = 8;
  static constexpr int kChunkSamples = 1600;

  std::atomic<int> chunks{0};

  [[nodiscard]] float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  [[nodiscard]] int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    for (int i = 0; i < kChunks && !input.cancellation.stop_requested(); ++i) {
      input.onChunk(std::vector<float>(kChunkSamples, 0.1F));
      ++chunks;
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
  }
};

struct SilentTts final : IVoiceTts
{
  [[nodiscard]] float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  [[nodiscard]] int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput) override {}
};

struct BurstTts final : IVoiceTts
{
  static constexpr int kChunks = 40;
  static constexpr int kChunkSamples = 1600;

  std::atomic<int> chunks{0};

  [[nodiscard]] float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  [[nodiscard]] int sampleRate(std::stop_token = {}) const override { return 16000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    for (int i = 0; i < kChunks && !input.cancellation.stop_requested(); ++i) {
      input.onChunk(std::vector<float>(kChunkSamples, 0.1F));
      ++chunks;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
};

struct QuietLlm final : IVoiceLlm
{
  void chatStream(LlmStreamInput input) override
  {
    if (input.request.prefillOnly)
      return;
    input.onToken("", true);
  }
};

struct ScriptedVadModel final : VadModel
{
  float probability(std::span<const float>) override { return 0.0F; }
  void reset() override {}
};

struct ScriptedVad final : IVoiceVad
{
  [[nodiscard]] std::unique_ptr<VadModel> createModel() const override
  {
    return std::make_unique<ScriptedVadModel>();
  }
};

class OpeningNone
{
public:
  OpeningNone() : previous_(voice_test_config::currentOpening())
  {
    voice_test_config::writeOpening(voiceOpeningToString(VoiceOpening::None));
  }

  ~OpeningNone()
  {
    try {
      voice_test_config::restoreOpening(previous_);
    } catch (...) {
      std::fprintf(stderr, "voice-test: could not restore the opening\n");
    }
  }

  OpeningNone(const OpeningNone&) = delete;
  OpeningNone& operator=(const OpeningNone&) = delete;

private:
  std::string previous_;
};

class GuardedContextAllocator final : public grpc::ContextAllocator
{
public:
  static constexpr std::size_t kArena = std::size_t{64} * 1024;

  grpc::CallbackServerContext* NewCallbackServerContext() override
  {
    void* block = ::mmap(nullptr, kArena, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED)
      return nullptr;
    void* placed = new (block) grpc::CallbackServerContext();
    auto* context = static_cast<grpc::CallbackServerContext*>(placed);
    {
      std::scoped_lock lock(mutex);
      spans.emplace(context, block);
    }
    return context;
  }

  void Release(grpc::CallbackServerContext* context) override
  {
    void* block = nullptr;
    {
      std::scoped_lock lock(mutex);
      const auto it = spans.find(context);
      if (it != spans.end()) {
        block = it->second;
        spans.erase(it);
      }
    }
    if (block == nullptr)
      return;
    released.fetch_add(1);
    context->~CallbackServerContext();
    ::mprotect(block, kArena, PROT_NONE);
  }

  std::atomic<int> released{0};

private:
  std::mutex mutex;
  std::unordered_map<grpc::CallbackServerContext*, void*> spans;
};

struct RunningServer
{
  std::unique_ptr<grpc::Server> server;
  std::string target;
};

RunningServer startServer(VoiceRpcService& service, GuardedContextAllocator* allocator)
{
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  if (allocator != nullptr)
    builder.SetContextAllocator(std::unique_ptr<grpc::ContextAllocator>(allocator));
  RunningServer running;
  running.server = builder.BuildAndStart();
  running.target = "127.0.0.1:" + std::to_string(port);
  return running;
}

struct ClientCall
{
  struct Input
  {
    const std::string& target;
    const std::string& secret;
  };

  explicit ClientCall(const Input& input)
  {
    channel = grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials());
    stub = argus::voice::v1::VoiceService::NewStub(channel);
    context.AddMetadata("x-argus-user", "1");
    context.AddMetadata("x-argus-role", "owner");
    argus::client::addCallerCredential(context, input.secret);
    stream = stub->Connect(&context);
  }

  void startCall()
  {
    argus::voice::v1::ClientFrame frame;
    auto* start = frame.mutable_start();
    start->mutable_identity()->set_user_id(7);
    start->mutable_identity()->set_role(argus::voice::v1::VOICE_ROLE_OWNER);
    start->mutable_identity()->set_language(argus::voice::v1::VOICE_LANGUAGE_ES);
    start->mutable_identity()->set_name("Ana");
    start->set_mode(argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
    CHECK(stream->Write(frame));
  }

  void drop()
  {
    context.TryCancel();
    std::ignore = stream->Finish();
  }

  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<argus::voice::v1::VoiceService::Stub> stub;
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<argus::voice::v1::ClientFrame,
                                           argus::voice::v1::ServerFrame>> stream;
};

bool announce(const ClientCall::Input& input, int64_t userId)
{
  auto stub = argus::voice::v1::VoiceService::NewStub(
      grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, input.secret);
  argus::voice::v1::AnnounceRequest request;
  request.set_user_id(userId);
  request.set_text("Hay alguien en el patio.");
  argus::voice::v1::AnnounceResponse reply;
  return stub->Announce(&context, request, &reply).ok() && reply.delivered();
}

VoiceCleanupDispatch throwingDispatch(std::atomic<int>& calls)
{
  return [&calls](const std::function<void()>&) {
    ++calls;
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                            "voice-test: the pool cannot start a worker");
  };
}

long rssKb()
{
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key != "VmRSS:")
      continue;
    long value = 0;
    std::string unit;
    status >> value >> unit;
    return value;
  }
  return 0;
}

}

struct VoiceSessionTestAccess
{
  static size_t sessions(VoiceSessionService& service)
  {
    std::scoped_lock lock(service.mutex_);
    return service.sessions_.size();
  }
};

TEST_CASE("A client that drops the stream mid-answer leaves no live voice stream behind")
{
  OpeningNone opening;
  SilentStt stt;
  PacedTts tts;
  QuietLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService sessions({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  auto allocator = std::make_unique<GuardedContextAllocator>();
  GuardedContextAllocator* contexts = allocator.get();
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = nullptr,
                           .dispatchCleanup = {}});
  RunningServer server = startServer(service, allocator.release());
  REQUIRE(server.server);

  for (int round = 0; round < 6; ++round) {
    const int chunksBefore = tts.chunks.load();
    ClientCall call({.target = server.target, .secret = kSyncSecret});
    call.startCall();
    REQUIRE(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 1; }, 3000));
    REQUIRE(announce({.target = server.target, .secret = kNotificationSecret}, 7));
    REQUIRE(waitFor([&] { return tts.chunks.load() > chunksBefore; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    call.drop();

    CHECK(waitFor([&] { return service.idle(); }, 5000));
    CHECK(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 0; }, 5000));
  }

  server.server->Shutdown();
  CHECK(contexts->released.load() >= 1);
}

TEST_CASE("A cleanup that cannot be queued still tears the stream down")
{
  OpeningNone opening;
  SilentStt stt;
  SilentTts tts;
  QuietLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService sessions({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  std::atomic<int> dispatches{0};
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = nullptr,
                           .dispatchCleanup = throwingDispatch(dispatches)});
  RunningServer server = startServer(service, nullptr);
  REQUIRE(server.server);

  for (int round = 0; round < 3; ++round) {
    ClientCall call({.target = server.target, .secret = kSyncSecret});
    call.startCall();
    REQUIRE(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 1; }, 3000));

    call.drop();

    CHECK(waitFor([&] { return service.idle(); }, 5000));
    CHECK(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 0; }, 5000));
  }
  CHECK(dispatches.load() >= 1);

  server.server->Shutdown();
}

TEST_CASE("A cancel racing a clean end completes the call exactly once")
{
  OpeningNone opening;
  SilentStt stt;
  PacedTts tts;
  QuietLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService sessions({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  auto allocator = std::make_unique<GuardedContextAllocator>();
  GuardedContextAllocator* contexts = allocator.get();
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = nullptr,
                           .dispatchCleanup = {}});
  RunningServer server = startServer(service, allocator.release());
  REQUIRE(server.server);

  std::seed_seq seed{20261008u};
  std::mt19937 rng(seed);
  for (int round = 0; round < 40; ++round) {
    const int chunksBefore = tts.chunks.load();
    ClientCall call({.target = server.target, .secret = kSyncSecret});
    call.startCall();
    REQUIRE(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 1; }, 3000));
    REQUIRE(announce({.target = server.target, .secret = kNotificationSecret}, 7));
    REQUIRE(waitFor([&] { return tts.chunks.load() > chunksBefore; }, 3000));

    const auto cancelDelay = std::chrono::microseconds(rng() % 4000);
    std::ignore = call.stream->WritesDone();
    std::thread ender([&call, cancelDelay] {
      std::this_thread::sleep_for(cancelDelay);
      call.context.TryCancel();
    });
    std::ignore = call.stream->Finish();
    ender.join();

    CHECK(waitFor([&] { return service.idle(); }, 5000));
    CHECK(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 0; }, 5000));
  }

  server.server->Shutdown();
  CHECK(contexts->released.load() >= 1);
}

TEST_CASE("Two hundred calls dropped at random points leave no live voice stream and the resident set bounded")
{
  OpeningNone opening;
  SilentStt stt;
  BurstTts tts;
  QuietLlm llm;
  FakeIdentity identity;
  ScriptedVad vad;
  VoiceSessionService sessions({.stt = stt, .tts = tts, .llm = llm, .identity = identity, .vad = vad});
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = nullptr,
                           .dispatchCleanup = {}});
  RunningServer server = startServer(service, nullptr);
  REQUIRE(server.server);

  std::seed_seq seed{4242u};
  std::mt19937 rng(seed);
  const auto oneRound = [&](int round) {
    CHECK_MESSAGE(waitFor([&] { return service.idle(); }, 1000),
                  "round ", round, " still had a live stream before it started");
    const unsigned mode = rng() % 4;
    ClientCall call({.target = server.target, .secret = kSyncSecret});
    CHECK_MESSAGE(waitFor([&] { return !service.idle(); }, 1000),
                  "round ", round, " mode ", mode, " never reached the server");
    if (mode > 0u) {
      call.startCall();
      if (mode > 1u) {
        if (waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 1; }, 1000))
          std::ignore = announce({.target = server.target, .secret = kNotificationSecret}, 7);
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 20));
      }
    }
    if (mode == 3u) {
      std::ignore = call.stream->WritesDone();
      std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 3));
    }
    call.drop();
    CHECK_MESSAGE(waitFor([&] { return service.idle(); }, 1000), "round ", round, " mode ", mode);
    CHECK_MESSAGE(waitFor([&] { return VoiceSessionTestAccess::sessions(sessions) == 0; }, 1000),
                  "round ", round, " mode ", mode);
  };

  for (int round = 0; round < 10; ++round)
    oneRound(round);
  const long before = rssKb();
  for (int round = 0; round < 200; ++round)
    oneRound(round);
  CHECK(waitFor([&] { return service.idle(); }, 5000));
  const long after = rssKb();
  MESSAGE("soak rss before=", before, " kB after=", after, " kB delta=", after - before, " kB");

  CHECK(VoiceSessionTestAccess::sessions(sessions) == 0);
  CHECK(after - before < 512 * 1024);
  server.server->Shutdown();
}
