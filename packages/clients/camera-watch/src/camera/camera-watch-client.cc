#include "camera-watch-client.hxx"

#include <grpc/grpc-client-base.hxx>

#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace
{

namespace v1 = argus::camera::v1;

std::string pcmBytes(std::span<const int16_t> pcm)
{
  std::string bytes;
  bytes.reserve(pcm.size() * 2);
  for (const int16_t sample : pcm) {
    const auto value = static_cast<uint16_t>(sample);
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  }
  return bytes;
}

class GrpcSpeakerStream final : public WatchSpeakerStream
{
public:
  GrpcSpeakerStream(v1::CameraWatchService::Stub& stub, const std::string& credential,
                    const WatchSpeakInput& input)
  {
    argus::client::addCallerCredential(context_, credential);
    io_ = stub.Speak(&context_);
    v1::SpeakFrame open;
    auto* body = open.mutable_open();
    body->set_camera_id(input.cameraId);
    body->set_sample_rate(input.sampleRate);
    body->set_earcon(input.earcon);
    body->set_conversation_id(input.conversationId);
    writable_ = io_->Write(open);
    reader_ = std::jthread([this, onEvent = input.onEvent] {
      v1::SpeakEvent event;
      while (io_->Read(&event)) {
        if (onEvent)
          onEvent(event);
      }
    });
  }

  GrpcSpeakerStream(const GrpcSpeakerStream&) = delete;
  GrpcSpeakerStream& operator=(const GrpcSpeakerStream&) = delete;

  ~GrpcSpeakerStream() override
  {
    bool pending = false;
    {
      std::scoped_lock lock(mutex_);
      pending = !finished_;
    }
    if (pending) {
      context_.TryCancel();
      static_cast<void>(finish());
    }
  }

  bool write(std::span<const int16_t> pcm) override
  {
    if (pcm.empty())
      return true;
    std::scoped_lock lock(mutex_);
    if (!writable_ || ended_)
      return false;
    v1::SpeakFrame frame;
    frame.set_pcm(pcmBytes(pcm));
    writable_ = io_->Write(frame);
    return writable_;
  }

  void end() override
  {
    std::scoped_lock lock(mutex_);
    endLocked();
  }

  void cancel() override { context_.TryCancel(); }

  grpc::Status finish() override
  {
    {
      std::scoped_lock lock(mutex_);
      if (finished_)
        return status_;
      endLocked();
    }
    if (reader_.joinable())
      reader_.join();
    std::scoped_lock lock(mutex_);
    if (!finished_) {
      status_ = io_->Finish();
      finished_ = true;
    }
    return status_;
  }

private:
  void endLocked()
  {
    if (ended_)
      return;
    ended_ = true;
    if (writable_) {
      v1::SpeakFrame frame;
      frame.mutable_end();
      writable_ = io_->Write(frame);
    }
    static_cast<void>(io_->WritesDone());
  }

  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderWriter<v1::SpeakFrame, v1::SpeakEvent>> io_;
  std::mutex mutex_;
  bool writable_{false};
  bool ended_{false};
  bool finished_{false};
  grpc::Status status_;
  std::jthread reader_;
};

}

WatchRefusal watchRefusalOf(const grpc::Status& status)
{
  if (status.ok())
    return WatchRefusal::WATCH_REFUSAL_NONE;
  if (status.error_code() != grpc::StatusCode::FAILED_PRECONDITION)
    return WatchRefusal::WATCH_REFUSAL_NONE;
  WatchRefusal refusal = WatchRefusal::WATCH_REFUSAL_NONE;
  if (!v1::WatchRefusal_Parse(status.error_message(), &refusal))
    return WatchRefusal::WATCH_REFUSAL_NONE;
  return refusal;
}

CameraWatchClient::CameraWatchClient(CameraWatchClientConfig config)
    : channel_(argus::client::makeStreamingChannel(config.target)),
      stub_(v1::CameraWatchService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::optional<std::vector<v1::WatchCamera>> CameraWatchClient::listWatch() const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kListTimeoutMs);
  argus::client::addCallerCredential(context, credential_);
  const v1::ListWatchRequest request;
  v1::ListWatchResponse response;
  if (const grpc::Status status = stub_->ListWatch(&context, request, &response); !status.ok())
    return std::nullopt;
  return std::vector<v1::WatchCamera>(response.cameras().begin(), response.cameras().end());
}

WatchMicrophoneEnd CameraWatchClient::readMicrophone(const WatchMicrophoneInput& input) const
{
  if (input.cameraId <= 0)
    return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "camera id is required"),
            .refusal = WatchRefusal::WATCH_REFUSAL_UNKNOWN_CAMERA,
            .stopped = false};
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, credential_);
  v1::MicrophoneRequest request;
  request.set_camera_id(input.cameraId);
  const std::stop_callback cancel(input.stop, [&context] { context.TryCancel(); });
  const auto reader = stub_->OpenMicrophone(&context, request);
  v1::MicrophoneFrame frame;
  while (reader->Read(&frame)) {
    if (input.onFrame)
      input.onFrame(frame);
  }
  const grpc::Status status = reader->Finish();
  return {.status = status, .refusal = watchRefusalOf(status), .stopped = input.stop.stop_requested()};
}

std::unique_ptr<WatchSpeakerStream> CameraWatchClient::speak(const WatchSpeakInput& input) const
{
  if (input.cameraId <= 0 || input.sampleRate <= 0)
    return nullptr;
  return std::make_unique<GrpcSpeakerStream>(*stub_, credential_, input);
}
