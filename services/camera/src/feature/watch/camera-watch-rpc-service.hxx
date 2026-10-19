#pragma once

#include <feature/watch/services/microphone-hub.hxx>
#include <feature/watch/services/watch-camera-board.hxx>

#include <argus/camera/v1/watch.grpc.pb.h>
#include <config/camera-config.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

struct CameraWatchServiceInput
{
  std::vector<argus::client::CallerCredential> callers;
  std::shared_ptr<MicrophoneHub> hub;
  std::shared_ptr<WatchCameraBoard> board;
  std::function<argus::camera::v1::WatchRefusal(int64_t)> gate;
  CameraWatchConfig config;
  std::function<std::optional<argus::identity::v1::IdentifyPersonResponse>(const CameraIdentifyInput&)> identify;
};

class WatchReactorRegistry
{
public:
  void add(uint64_t id, std::shared_ptr<void> reactor);
  [[nodiscard]] std::shared_ptr<void> take(uint64_t id);
  [[nodiscard]] size_t size() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<void>> live_;
};

class CameraWatchRpcService final : public argus::camera::v1::CameraWatchService::CallbackService
{
public:
  explicit CameraWatchRpcService(CameraWatchServiceInput input);

  grpc::ServerUnaryReactor* ListWatch(grpc::CallbackServerContext* context,
                                      const argus::camera::v1::ListWatchRequest* request,
                                      argus::camera::v1::ListWatchResponse* response) override;

  grpc::ServerWriteReactor<argus::camera::v1::MicrophoneFrame>*
  OpenMicrophone(grpc::CallbackServerContext* context, const argus::camera::v1::MicrophoneRequest* request) override;

  grpc::ServerBidiReactor<argus::camera::v1::SpeakFrame, argus::camera::v1::SpeakEvent>*
  Speak(grpc::CallbackServerContext* context) override;

  grpc::ServerUnaryReactor* FaceWindow(grpc::CallbackServerContext* context,
                                       const argus::camera::v1::FaceWindowRequest* request,
                                       argus::camera::v1::FaceWindowResponse* response) override;

  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  [[nodiscard]] bool authorized(const grpc::CallbackServerContext* context) const;

  std::vector<argus::client::CallerCredential> callers_;
  std::shared_ptr<MicrophoneHub> hub_;
  std::shared_ptr<WatchCameraBoard> board_;
  std::function<argus::camera::v1::WatchRefusal(int64_t)> gate_;
  CameraWatchConfig config_;
  std::function<std::optional<argus::identity::v1::IdentifyPersonResponse>(const CameraIdentifyInput&)> identify_;
  std::shared_ptr<WatchReactorRegistry> reactors_{std::make_shared<WatchReactorRegistry>()};
  std::atomic<uint64_t> nextId_{1};
  std::atomic<bool> stopping_{false};
};
