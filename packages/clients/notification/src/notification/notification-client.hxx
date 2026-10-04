#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

enum class NotificationRpcOutcome : uint8_t
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

struct NotificationCallClaimInput
{
  std::string callId;
  int64_t userId{0};
  std::string sessionId;
};

struct NotificationCallClaimResult
{
  NotificationRpcOutcome outcome{NotificationRpcOutcome::Unavailable};
  grpc::Status status;
  argus::notification::v1::ClaimCallResponse response;
};

struct NotificationCallEndInput
{
  std::string callId;
  int64_t userId{0};
  argus::notification::v1::CallOutcome outcome{
      argus::notification::v1::CALL_OUTCOME_COMPLETED};
  bool spoken{false};
};

struct NotificationCallScheduleInput
{
  int64_t userId{0};
  int64_t fireAt{0};
  std::string topic;
  std::string lang;
  std::string commandId;
};

struct NotificationCallScheduleResult
{
  NotificationRpcOutcome outcome{NotificationRpcOutcome::Unavailable};
  grpc::Status status;
  int64_t scheduledId{0};
  bool duplicate{false};
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

  [[nodiscard]] virtual NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity& identity) const;

  [[nodiscard]] virtual NotificationPullResult pullNotifications(
      const argus::notification::v1::PullNotificationsRequest& request,
      const argus::client::CallerIdentity& identity) const;

  [[nodiscard]] virtual NotificationCallClaimResult
  claimCall(const NotificationCallClaimInput& input) const;

  [[nodiscard]] virtual NotificationRpcOutcome
  endCall(const NotificationCallEndInput& input) const;

  [[nodiscard]] virtual NotificationCallScheduleResult
  scheduleCall(const NotificationCallScheduleInput& input) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::string credential_;
  std::unique_ptr<argus::notification::v1::NotificationService::StubInterface>
      stub_;
  std::unique_ptr<argus::notification::v1::CallService::StubInterface>
      callStub_;
};
