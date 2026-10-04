#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <feature/call/services/call-engine.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <notification/notification-delivery-sink.hxx>
#include <nats/push-intent-sink.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <vector>

class NotificationRpcService final
    : public argus::notification::v1::NotificationService::CallbackService
{
public:
  using Dependencies = NotificationService::Dependencies;

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

  void startDeliveryReconciler();

  void startSelfTestProber();

  void attachCallEngine(std::shared_ptr<const CallEngine> engine);

private:
  std::vector<argus::client::CallerCredential> guardCallers_;
  std::vector<argus::client::CallerCredential> syncCallers_;
  std::shared_ptr<const CallEngine> callEngine_;
  NotificationService notificationService_;
  NotificationRepository repository_;
};
