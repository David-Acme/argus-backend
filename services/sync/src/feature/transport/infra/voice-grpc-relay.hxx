#pragma once

#include <chrono>
#include <string_view>

#include <argus/voice/v1/voice.grpc.pb.h>
#include <drogon/WebSocketController.h>
#include <sync/sync-forwarder.hxx>
#include <grpcpp/grpcpp.h>
#include <json/value.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <auth/user-directory.hxx>
#include <auth/user-role.hxx>
#include <voice/voice-client.hxx>

struct VoiceGrpcConfig
{
  std::string target;
  std::string credential;

  static VoiceGrpcConfig resolve();
};

class VoiceGrpcRelay final : public SyncForwarder
{
public:
  VoiceGrpcRelay(VoiceGrpcConfig config,
                 std::shared_ptr<const IUserDirectory> directory);

  void onConnect(const drogon::HttpRequestPtr& req,
                 const drogon::WebSocketConnectionPtr& conn) override;
  drogon::Task<bool> forwardText(const SyncFrameInput& input) override;
  void forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& data) override;
  void onClose(const drogon::WebSocketConnectionPtr& conn) override;

  std::chrono::milliseconds farewell(const drogon::WebSocketConnectionPtr& conn, std::string_view cause);

  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr auto kFarewellGrace = std::chrono::milliseconds(2300);

  static Json::Value renderServerFrame(
      const argus::voice::v1::ServerFrame& frame);
  static argus::voice::v1::VoiceMode startModeOf(const Json::Value& message);
  static bool resumeOf(const Json::Value& message);
  static argus::voice::v1::VoiceContext contextOf(const Json::Value& payload);
  static argus::voice::v1::VoiceActionResult actionResultOf(const Json::Value& payload);
  static bool mutedOf(const Json::Value& payload);

private:
  class StreamObserver;
  using StreamOp = std::function<void(VoiceStream&)>;

  struct Session
  {
    int64_t userId{0};
    UserRole role{UserRole::Unknown};
    std::string deviceHash;
    trantor::EventLoop* loop{nullptr};
    std::atomic<bool> closing{false};
    std::atomic<bool> revoked{false};
    std::atomic<bool> inCall{false};
    std::mutex mutex;
    std::shared_ptr<VoiceStream> stream;
    bool starting{false};
    std::vector<StreamOp> pending;
  };

  struct StartInput
  {
    const drogon::WebSocketConnectionPtr& conn;
    const std::shared_ptr<Session>& session;
    argus::voice::v1::VoiceMode mode{argus::voice::v1::VOICE_MODE_HALF_DUPLEX};
    bool resume{false};
  };

  static void deliver(Session& session, StreamOp op);
  drogon::Task<void> startStream(StartInput input);
  std::shared_ptr<Session> sessionFor(const drogon::WebSocketConnectionPtr& conn);
  std::shared_ptr<Session> takeSession(const drogon::WebSocketConnectionPtr& conn);

  const std::shared_ptr<VoiceClient> client_;
  const std::shared_ptr<const IUserDirectory> userDirectory_;
  mutable std::mutex sessionsMutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
  const std::shared_ptr<std::atomic<int>> openStreams_{
      std::make_shared<std::atomic<int>>(0)};
  std::atomic<bool> stopping_{false};
};
