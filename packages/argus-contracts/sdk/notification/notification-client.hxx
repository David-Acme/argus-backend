#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

// Thin SDK wrapper over argus.notification.v1.NotificationService (rule 23).
class NotificationClient
{
public:
  explicit NotificationClient(std::string target);

  NotificationClient(const NotificationClient&) = delete;
  NotificationClient& operator=(const NotificationClient&) = delete;
  virtual ~NotificationClient() = default;

  // Fan-out create; nullopt when argus-notification refuses or is unreachable.
  virtual std::optional<argus::notification::v1::CreateNotificationsResponse>
  createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity& identity) const;

  // User-scoped notification pull; nullopt when the owner refuses.
  virtual std::optional<argus::notification::v1::PullNotificationsResponse>
  pullNotifications(
      const argus::notification::v1::PullNotificationsRequest& request,
      const argus::sdk::CallerIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::notification::v1::NotificationService::StubInterface>
      stub_;
};
