#pragma once

#include <argus/voice/v1/voice.grpc.pb.h>
#include <feature/voice/voice-session-service.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
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
  virtual void farewellRoom(const argus::voice::v1::RtcFarewell& farewell, std::function<void()> done) = 0;
};

using VoiceCleanupDispatch = std::function<void(std::function<void()>)>;

struct WatchReplyRequest
{
  int64_t userId{0};
  std::string conversationId;
  std::string text;
};

class VoiceWatchDirectory
{
public:
  virtual ~VoiceWatchDirectory() = default;

  [[nodiscard]] virtual std::vector<argus::voice::v1::WatchConversation> conversations(int64_t userId) const = 0;
  [[nodiscard]] virtual bool end(int64_t userId, const std::string& conversationId) = 0;
  [[nodiscard]] virtual argus::voice::v1::ReplyWatchVisitorResponse reply(const WatchReplyRequest& request) = 0;
};

struct VoiceRpcInput
{
  VoiceSessionService* sessions{nullptr};
  std::string syncCallerSecret;
  std::string notificationCallerSecret;
  VoiceRoomJoiner* rooms{nullptr};
  VoiceCleanupDispatch dispatchCleanup;
  VoiceWatchDirectory* watch{nullptr};
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

  grpc::ServerUnaryReactor*
  ListWatchConversations(grpc::CallbackServerContext* context,
                         const argus::voice::v1::ListWatchConversationsRequest* request,
                         argus::voice::v1::ListWatchConversationsResponse* reply) override;

  grpc::ServerUnaryReactor*
  EndWatchConversation(grpc::CallbackServerContext* context,
                       const argus::voice::v1::EndWatchConversationRequest* request,
                       argus::voice::v1::EndWatchConversationResponse* reply) override;

  grpc::ServerUnaryReactor*
  ReplyWatchVisitor(grpc::CallbackServerContext* context,
                    const argus::voice::v1::ReplyWatchVisitorRequest* request,
                    argus::voice::v1::ReplyWatchVisitorResponse* reply) override;

  [[nodiscard]] bool idle() const { return live_->load() == 0; }

private:
  struct Streams;

  VoiceSessionService& sessions_;
  std::vector<argus::client::CallerCredential> syncCallers_;
  std::vector<argus::client::CallerCredential> notificationCallers_;
  VoiceRoomJoiner* rooms_;
  std::shared_ptr<std::atomic<int>> live_{std::make_shared<std::atomic<int>>(0)};
  std::atomic<std::uint64_t> nextStreamId_{1};
  std::shared_ptr<Streams> streams_;
  VoiceCleanupDispatch dispatchCleanup_;
  VoiceWatchDirectory* watch_{nullptr};
};
