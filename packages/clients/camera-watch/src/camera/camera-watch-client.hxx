#pragma once

#include <argus/camera/v1/watch.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

using WatchRefusal = argus::camera::v1::WatchRefusal;
using SpeakState = argus::camera::v1::SpeakState;

struct CameraWatchClientConfig
{
  std::string target;
  std::string credential;
};

struct WatchMicrophoneInput
{
  int64_t cameraId{0};
  std::function<void(const argus::camera::v1::MicrophoneFrame&)> onFrame;
  std::stop_token stop;
};

struct WatchMicrophoneEnd
{
  grpc::Status status;
  WatchRefusal refusal{WatchRefusal::WATCH_REFUSAL_NONE};
  bool stopped{false};
};

struct WatchSpeakInput
{
  int64_t cameraId{0};
  int sampleRate{16000};
  bool earcon{false};
  std::string conversationId;
  std::function<void(const argus::camera::v1::SpeakEvent&)> onEvent;
};

class WatchSpeakerStream
{
public:
  virtual ~WatchSpeakerStream() = default;

  [[nodiscard]] virtual bool write(std::span<const int16_t> pcm) = 0;
  virtual void end() = 0;
  virtual void cancel() = 0;
  virtual grpc::Status finish() = 0;
};

[[nodiscard]] WatchRefusal watchRefusalOf(const grpc::Status& status);

class CameraWatchClient
{
public:
  explicit CameraWatchClient(CameraWatchClientConfig config);

  CameraWatchClient(const CameraWatchClient&) = delete;
  CameraWatchClient& operator=(const CameraWatchClient&) = delete;
  virtual ~CameraWatchClient() = default;

  [[nodiscard]] virtual std::optional<std::vector<argus::camera::v1::WatchCamera>> listWatch() const;

  virtual WatchMicrophoneEnd readMicrophone(const WatchMicrophoneInput& input) const;

  [[nodiscard]] virtual std::unique_ptr<WatchSpeakerStream> speak(const WatchSpeakInput& input) const;

  [[nodiscard]] virtual std::optional<argus::camera::v1::FaceWindowResponse> faceWindow(int64_t cameraId,
                                                                                        int windowMs) const;

  static constexpr int kListTimeoutMs = 3000;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::camera::v1::CameraWatchService::Stub> stub_;
  std::string credential_;
};
