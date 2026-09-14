#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// Transport result kept distinct from a durable command conflict, so a reused
// command id is never retried as if the service were merely unreachable.
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

// The caller-side capability credential for the caller -> notification edge.
struct NotificationClientConfig
{
  std::string target;
  std::string credential;
};

// Thin SDK wrapper over argus.notification.v1.NotificationService (rule 23).
class NotificationClient
{
public:
  explicit NotificationClient(NotificationClientConfig config);

  NotificationClient(const NotificationClient&) = delete;
  NotificationClient& operator=(const NotificationClient&) = delete;
  virtual ~NotificationClient() = default;

  virtual NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity& identity) const;

  virtual NotificationPullResult pullNotifications(
      const argus::notification::v1::PullNotificationsRequest& request,
      const argus::sdk::CallerIdentity& identity) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::string credential_;
  std::unique_ptr<argus::notification::v1::NotificationService::StubInterface>
      stub_;
};
