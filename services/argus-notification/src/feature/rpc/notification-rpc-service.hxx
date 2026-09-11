#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/notification/notification-service.hxx>

// argus.notification.v1.NotificationService: create fan-out and user pulls.
class NotificationRpcService final
    : public argus::notification::v1::NotificationService::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  CreateNotifications(grpc::CallbackServerContext* context,
                      const argus::notification::v1::CreateNotificationsRequest*
                          request,
                      argus::notification::v1::CreateNotificationsResponse*
                          response) override;

  grpc::ServerUnaryReactor*
  PullNotifications(grpc::CallbackServerContext* context,
                    const argus::notification::v1::PullNotificationsRequest*
                        request,
                    argus::notification::v1::PullNotificationsResponse*
                        response) override;

private:
  NotificationService notificationService_;
  NotificationRepository repository_;
};
