#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <vector>

// argus.notification.v1.NotificationService: create fan-out and user pulls.
class NotificationRpcService final
    : public argus::notification::v1::NotificationService::CallbackService
{
public:
  struct Dependencies
  {
    std::shared_ptr<const NotificationDeliverySink> deliverySink;
    std::shared_ptr<const push_intent::PushIntentSink> pushSink;
    bool pushRequired{false};
  };

  // Each RPC is gated by the capability credential of its single caller.
  NotificationRpcService() = default;
  explicit NotificationRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor* CreateNotifications(
      grpc::CallbackServerContext* context,
      const argus::notification::v1::CreateNotificationsRequest* request,
      argus::notification::v1::CreateNotificationsResponse* response) override;

  grpc::ServerUnaryReactor* PullNotifications(
      grpc::CallbackServerContext* context,
      const argus::notification::v1::PullNotificationsRequest* request,
      argus::notification::v1::PullNotificationsResponse* response) override;

  // Startup + periodic reconciliation of durable delivery intents.
  void startDeliveryReconciler();

private:
  std::vector<argus::sdk::CallerCredential> guardCallers_;
  std::vector<argus::sdk::CallerCredential> gatewayCallers_;
  NotificationService notificationService_;
  NotificationRepository repository_;
};
