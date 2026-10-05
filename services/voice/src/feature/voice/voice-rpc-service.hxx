#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <feature/voice/voice-session-service.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class VoiceRoomJoiner
{
public:
  virtual ~VoiceRoomJoiner() = default;

  virtual void joinRoom(const argus::voice::v1::RtcJoin& join,
                        std::function<void(grpc::Status, argus::voice::v1::RtcJoined)> done) = 0;
  virtual void farewellRoom(const argus::voice::v1::RtcFarewell& farewell, std::function<void(bool)> done) = 0;
};

struct VoiceRpcInput
{
  VoiceSessionService* sessions{nullptr};
  std::string syncCallerSecret;
  std::string notificationCallerSecret;
  VoiceRoomJoiner* rooms{nullptr};
};

class VoiceRpcService final
    : public argus::voice::v1::VoiceService::CallbackService
{
public:
  explicit VoiceRpcService(VoiceRpcInput input);

  grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                          argus::voice::v1::ServerFrame>*
  Connect(grpc::CallbackServerContext* context) override;

  grpc::ServerUnaryReactor* JoinRoom(grpc::CallbackServerContext* context,
                                     const argus::voice::v1::RtcJoin* request,
                                     argus::voice::v1::RtcJoined* reply) override;

  grpc::ServerUnaryReactor* Farewell(grpc::CallbackServerContext* context,
                                     const argus::voice::v1::RtcFarewell* request,
                                     argus::voice::v1::RtcFarewellDone* reply) override;

  grpc::ServerUnaryReactor* Announce(grpc::CallbackServerContext* context,
                                     const argus::voice::v1::AnnounceRequest* request,
                                     argus::voice::v1::AnnounceResponse* reply) override;

  [[nodiscard]] bool idle() const { return live_->load() == 0; }

private:
  VoiceSessionService& sessions_;
  std::vector<argus::client::CallerCredential> syncCallers_;
  std::vector<argus::client::CallerCredential> notificationCallers_;
  VoiceRoomJoiner* rooms_;
  std::shared_ptr<std::atomic<int>> live_{std::make_shared<std::atomic<int>>(0)};
};
