#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <feature/call/services/call-engine.hxx>
#include <feature/conversation/repositories/conversation-message/conversation-message-repository.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <notification/notification-delivery-sink.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/task-gate/task-gate.hxx>
#include <vector>

class NotificationRpcService final
    : public argus::notification::v1::NotificationService::CallbackService
{
public:
  explicit NotificationRpcService(std::shared_ptr<const NotificationService> notifications);

  grpc::ServerUnaryReactor* CreateNotifications(
      grpc::CallbackServerContext* context,
      const argus::notification::v1::CreateNotificationsRequest* request,
      argus::notification::v1::CreateNotificationsResponse* response) override;

  grpc::ServerUnaryReactor* PullNotifications(
      grpc::CallbackServerContext* context,
      const argus::notification::v1::PullNotificationsRequest* request,
      argus::notification::v1::PullNotificationsResponse* response) override;

  grpc::ServerUnaryReactor* PullConversation(
      grpc::CallbackServerContext* context,
      const argus::notification::v1::PullConversationRequest* request,
      argus::notification::v1::PullConversationResponse* response) override;

  void startDeliveryReconciler();

  drogon::Task<void> bootSelfTest() const;
  void startSelfTestProber();

  void attachCallEngine(std::shared_ptr<const CallEngine> engine);

  void attachTasks(std::shared_ptr<TaskGate> tasks);

private:
  std::vector<argus::client::CallerCredential> guardCallers_;
  std::vector<argus::client::CallerCredential> assistantCallers_;
  std::vector<argus::client::CallerCredential> syncCallers_;
  std::shared_ptr<const CallEngine> callEngine_;
  std::shared_ptr<TaskGate> tasks_;
  std::shared_ptr<const NotificationService> notificationService_;
  NotificationRepository repository_;
  ConversationMessageRepository conversations_;
};
