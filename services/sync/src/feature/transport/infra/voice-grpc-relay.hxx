#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <drogon/WebSocketController.h>
#include <sync/sync-forwarder.hxx>
#include <grpcpp/grpcpp.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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

  static Json::Value renderServerFrame(
      const argus::voice::v1::ServerFrame& frame);

private:
  class StreamObserver;
  struct Session
  {
    int64_t userId{0};
    UserRole role{UserRole::Guest};
    trantor::EventLoop* loop{nullptr};
    std::shared_ptr<VoiceStream> stream;
    bool failed{false};
    bool closing{false};
  };

  std::shared_ptr<Session> sessionFor(const drogon::WebSocketConnectionPtr& conn);
  std::shared_ptr<Session> takeSession(const drogon::WebSocketConnectionPtr& conn);

  const std::shared_ptr<VoiceClient> client_;
  const std::shared_ptr<const IUserDirectory> userDirectory_;
  mutable std::mutex sessionsMutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
};
