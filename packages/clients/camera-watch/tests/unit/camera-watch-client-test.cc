#include <argus/camera/v1/watch.grpc.pb.h>
#include <camera/camera-watch-client.hxx>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <test-support/value-of.hxx>

namespace
{

namespace v1 = argus::camera::v1;

class ScriptedWatchService final : public v1::CameraWatchService::Service
{
public:
  grpc::Status ListWatch(grpc::ServerContext*, const v1::ListWatchRequest*,
                         v1::ListWatchResponse* response) override
  {
    auto* camera = response->add_cameras();
    camera->set_camera_id(7);
    camera->set_name("Sala");
    camera->set_listening(true);
    camera->set_talk(true);
    return grpc::Status::OK;
  }

  grpc::Status OpenMicrophone(grpc::ServerContext*, const v1::MicrophoneRequest* request,
                              grpc::ServerWriter<v1::MicrophoneFrame>* writer) override
  {
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
      v1::MicrophoneFrame frame;
      frame.set_sequence(sequence);
      frame.set_pcm(std::string(4, static_cast<char>(request->camera_id())));
      writer->Write(frame);
    }
    return {grpc::StatusCode::FAILED_PRECONDITION,
            v1::WatchRefusal_Name(v1::WATCH_REFUSAL_MASKED)};
  }

  grpc::Status Speak(grpc::ServerContext*,
                     grpc::ServerReaderWriter<v1::SpeakEvent, v1::SpeakFrame>* stream) override
  {
    v1::SpeakFrame frame;
    while (stream->Read(&frame)) {
      std::scoped_lock lock(mutex);
      if (frame.has_open()) {
        cameraId = frame.open().camera_id();
        v1::SpeakEvent ready;
        ready.set_state(v1::SPEAK_STATE_READY);
        stream->Write(ready);
      }
      else if (frame.body_case() == v1::SpeakFrame::kPcm) {
        bytes += frame.pcm().size();
      }
      else if (frame.has_end()) {
        ended = true;
      }
    }
    v1::SpeakEvent done;
    done.set_state(v1::SPEAK_STATE_DONE);
    done.set_played_ms(1000);
    stream->Write(done);
    return grpc::Status::OK;
  }

  std::mutex mutex;
  int64_t cameraId{0};
  size_t bytes{0};
  bool ended{false};
};

struct Served
{
  std::unique_ptr<grpc::Server> server;
  int port{0};
};

Served serve(ScriptedWatchService& service)
{
  Served served;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &served.port);
  builder.RegisterService(&service);
  served.server = builder.BuildAndStart();
  return served;
}

}

TEST_CASE("a refusal travels as its enum name and only on a failed precondition")
{
  CHECK(watchRefusalOf(grpc::Status::OK) == v1::WATCH_REFUSAL_NONE);
  CHECK(watchRefusalOf({grpc::StatusCode::FAILED_PRECONDITION, "WATCH_REFUSAL_NO_AUDIO"}) ==
        v1::WATCH_REFUSAL_NO_AUDIO);
  CHECK(watchRefusalOf({grpc::StatusCode::UNAVAILABLE, "WATCH_REFUSAL_NO_AUDIO"}) ==
        v1::WATCH_REFUSAL_NONE);
  CHECK(watchRefusalOf({grpc::StatusCode::FAILED_PRECONDITION, "something else"}) ==
        v1::WATCH_REFUSAL_NONE);
}

TEST_CASE("the microphone feed delivers every frame and then the camera's refusal")
{
  ScriptedWatchService service;
  const Served served = serve(service);
  REQUIRE(served.server != nullptr);
  const CameraWatchClient client({.target = "127.0.0.1:" + std::to_string(served.port), .credential = "k"});

  std::vector<uint64_t> sequences;
  const WatchMicrophoneEnd end = client.readMicrophone(
      {.cameraId = 5,
       .onFrame = [&sequences](const v1::MicrophoneFrame& frame) { sequences.push_back(frame.sequence()); },
       .stop = {}});
  CHECK(sequences == std::vector<uint64_t>{1, 2, 3});
  CHECK(end.refusal == v1::WATCH_REFUSAL_MASKED);
  CHECK_FALSE(end.stopped);

  const auto cameras = valueOf(client.listWatch());
  REQUIRE(cameras.size() == 1);
  CHECK(cameras.front().name() == "Sala");

  const WatchMicrophoneEnd refused = client.readMicrophone({.cameraId = 0, .onFrame = {}, .stop = {}});
  CHECK(refused.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  served.server->Shutdown();
}

TEST_CASE("the speaker stream opens, carries little-endian samples and reports done")
{
  ScriptedWatchService service;
  const Served served = serve(service);
  REQUIRE(served.server != nullptr);
  const CameraWatchClient client({.target = "127.0.0.1:" + std::to_string(served.port), .credential = "k"});

  std::mutex mutex;
  std::vector<v1::SpeakState> states;
  auto stream = client.speak({.cameraId = 9,
                              .sampleRate = 16000,
                              .earcon = true,
                              .conversationId = "c1",
                              .onEvent = [&](const v1::SpeakEvent& event) {
                                std::scoped_lock lock(mutex);
                                states.push_back(event.state());
                              }});
  REQUIRE(stream != nullptr);
  const std::array<int16_t, 4> samples{1, -1, 300, -300};
  CHECK(stream->write(samples));
  stream->end();
  CHECK(stream->finish().ok());
  {
    std::scoped_lock lock(service.mutex);
    CHECK(service.cameraId == 9);
    CHECK(service.bytes == 8);
    CHECK(service.ended);
  }
  std::scoped_lock lock(mutex);
  REQUIRE(states.size() == 2);
  CHECK(states.front() == v1::SPEAK_STATE_READY);
  CHECK(states.back() == v1::SPEAK_STATE_DONE);
  CHECK(client.speak({.cameraId = 0, .sampleRate = 16000, .earcon = false, .conversationId = {}, .onEvent = {}}) == nullptr);
  served.server->Shutdown();
}
