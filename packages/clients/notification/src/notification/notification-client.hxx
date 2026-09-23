#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

enum class NotificationRpcOutcome
{
  Success,
  Conflict,
  Rejected,
  Unavailable,
};

struct NotificationCreateResult
{
  NotificationRpcOutcome outcome{NotificationRpcOutcome::Unavailable};
  grpc::Status status;
  int32_t created{0};
  bool duplicate{false};
};

struct NotificationPullResult
{
  NotificationRpcOutcome outcome{NotificationRpcOutcome::Unavailable};
  grpc::Status status;
  argus::notification::v1::PullNotificationsResponse response;
};

struct NotificationClientConfig
{
  std::string target;
  std::string credential;
};

class NotificationClient
{
public:
  explicit NotificationClient(NotificationClientConfig config);

  NotificationClient(const NotificationClient&) = delete;
  NotificationClient& operator=(const NotificationClient&) = delete;
  virtual ~NotificationClient() = default;

  virtual NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity& identity) const;

  virtual NotificationPullResult pullNotifications(
      const argus::notification::v1::PullNotificationsRequest& request,
      const argus::client::CallerIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::string credential_;
  std::unique_ptr<argus::notification::v1::NotificationService::StubInterface>
      stub_;
};
