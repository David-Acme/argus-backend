#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/voice/v1/voice.grpc.pb.h>
#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <voice/voice-client.hxx>

namespace
{
namespace v1 = argus::voice::v1;

class CollectingObserver final : public VoiceStreamObserver
{
public:
  void onServerFrame(v1::ServerFrame frame) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    frames.push_back(std::move(frame));
  }
  void onStreamClosed(const grpc::Status& status) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    closeStatus = status;
    cv.notify_all();
  }

  bool waitClosed(int timeoutMs)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                       [this] { return closed; });
  }

  std::vector<v1::ServerFrame> frames;
  bool closed{false};
  grpc::Status closeStatus;
  std::mutex mutex;
  std::condition_variable cv;
};

class RecordingVoiceService final : public v1::VoiceService::Service
{
public:
  using FrameStream =
      grpc::ServerReaderWriter<v1::ServerFrame, v1::ClientFrame>;

  grpc::Status Connect(grpc::ServerContext* context,
                       FrameStream* stream) override
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      context_ = context;
      for (const auto& [key, value] : context->client_metadata())
        metadata.emplace(std::string(key.begin(), key.end()),
                         std::string(value.begin(), value.end()));
    }
    v1::ClientFrame frame;
    bool answered = false;
    while (stream->Read(&frame)) {
      const bool started = frame.has_start();
      {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame);
      }
      if (started && !answered) {
        answered = true;
        v1::ServerFrame done;
        done.mutable_done()->set_session_id(4242);
        stream->Write(done);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      context_ = nullptr;
    }
    return grpc::Status::OK;
  }

  void cancelActiveCall()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (context_ != nullptr)
      context_->TryCancel();
  }

  std::vector<v1::ClientFrame> frames;
  std::map<std::string, std::string> metadata;
  std::mutex mutex;
  grpc::ServerContext* context_{nullptr};
};

std::unique_ptr<grpc::Server> startServer(RecordingVoiceService& service,
                                          int& port)
{
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  return std::unique_ptr<grpc::Server>(builder.BuildAndStart());
}

struct CallCleanup
{
  RecordingVoiceService& service;
  grpc::Server& server;

  ~CallCleanup()
  {
    service.cancelActiveCall();
    server.Shutdown(std::chrono::system_clock::now() +
                    std::chrono::milliseconds(500));
  }
};
}

TEST_CASE("the role string is the one a receiver gates on")
{
  CHECK(voiceRoleToString(v1::VOICE_ROLE_OWNER) == "owner");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_RESIDENT) == "resident");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_GUARD) == "guard");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_GUEST) == "guest");
  CHECK(voiceRoleToString(static_cast<v1::VoiceRole>(42)) == "guest");
}

TEST_CASE("one stream carries the connect identity and the frames in order")
{
  RecordingVoiceService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  VoiceClient client(
      {.target = "127.0.0.1:" + std::to_string(port), .credential = ""});
  CHECK(client.waitConnected(5000));
  CallCleanup cleanup{service, *server};

  const auto observer = std::make_shared<CollectingObserver>();

  v1::VoiceIdentity connectIdentity;
  connectIdentity.set_user_id(7);
  connectIdentity.set_role(v1::VOICE_ROLE_RESIDENT);
  const auto stream = client.connect(connectIdentity, observer);

  v1::VoiceStart start;
  start.mutable_identity()->set_user_id(9);
  start.mutable_identity()->set_name("Ana");
  start.mutable_identity()->set_role(v1::VOICE_ROLE_OWNER);
  start.set_mode(v1::VOICE_MODE_DUPLEX);
  stream->start(start);
  stream->sendPcm("a\0b", 3);
  stream->skip();
  stream->stop();
  stream->finish();
  REQUIRE(observer->waitClosed(5000));

  CHECK(observer->closeStatus.ok());
  REQUIRE(observer->frames.size() == 1);
  CHECK(observer->frames[0].done().session_id() == 4242);

  std::lock_guard<std::mutex> lock(service.mutex);
  CHECK(service.metadata.at("x-argus-user") == "7");
  CHECK(service.metadata.at("x-argus-role") == "resident");
  CHECK(service.metadata.count("x-argus-device") == 0);
  CHECK(service.metadata.count("x-argus-credential") == 0);
  REQUIRE(service.frames.size() == 4);
  REQUIRE(service.frames[0].has_start());
  CHECK(service.frames[0].start().identity().user_id() == 9);
  CHECK(service.frames[0].start().identity().role() == v1::VOICE_ROLE_OWNER);
  CHECK(service.frames[0].start().mode() == v1::VOICE_MODE_DUPLEX);
  CHECK(service.frames[1].pcm() == std::string("a\0b", 3));
  CHECK(service.frames[2].has_skip());
  CHECK(service.frames[3].has_stop());
}
