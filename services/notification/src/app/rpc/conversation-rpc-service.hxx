#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <feature/conversation/services/conversation-inbox-service.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <vector>

class ConversationRpcService final : public argus::notification::v1::ConversationService::CallbackService
{
public:
  explicit ConversationRpcService(std::shared_ptr<const ConversationInboxService> inbox);

  grpc::ServerUnaryReactor* RelayMessage(grpc::CallbackServerContext* context,
                                         const argus::notification::v1::RelayMessageRequest* request,
                                         argus::notification::v1::RelayMessageResponse* response) override;

  grpc::ServerUnaryReactor* SettleMessage(grpc::CallbackServerContext* context,
                                          const argus::notification::v1::SettleMessageRequest* request,
                                          argus::notification::v1::SettleMessageResponse* response) override;

  grpc::ServerUnaryReactor* AppendTurns(grpc::CallbackServerContext* context,
                                        const argus::notification::v1::AppendTurnsRequest* request,
                                        argus::notification::v1::AppendTurnsResponse* response) override;

private:
  [[nodiscard]] bool admitted(grpc::CallbackServerContext* context) const;

  std::vector<argus::client::CallerCredential> assistantCallers_;
  std::shared_ptr<const ConversationInboxService> inbox_;
};

[[nodiscard]] std::optional<RelayLetter> relayLetterOf(const argus::notification::v1::RelayMessageRequest& request);

[[nodiscard]] std::optional<SettleLetter> settleLetterOf(const argus::notification::v1::SettleMessageRequest& request);

[[nodiscard]] std::optional<TranscriptInput> transcriptOf(const argus::notification::v1::AppendTurnsRequest& request);
