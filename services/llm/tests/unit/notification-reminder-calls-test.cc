#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/infra/notification-reminder-calls.hxx>

#include <grpcpp/support/status.h>

#include <memory>
#include <string>

namespace
{
class ProgrammedNotifications final : public NotificationClient
{
public:
  ProgrammedNotifications(NotificationRpcOutcome outcome, grpc::Status status)
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}}), outcome_(outcome), status_(std::move(status))
  {
  }

  [[nodiscard]] NotificationCallScheduleResult scheduleCall(const NotificationCallScheduleInput&) const override
  {
    ++asked;
    return {.outcome = outcome_, .status = status_, .scheduledId = 0, .duplicate = false};
  }

  mutable int asked{0};

private:
  NotificationRpcOutcome outcome_;
  grpc::Status status_;
};

ReminderCallOutcome answered(NotificationRpcOutcome outcome, const grpc::Status& status)
{
  const auto client = std::make_shared<ProgrammedNotifications>(outcome, status);
  const NotificationReminderCalls calls(client);
  const ReminderCallOutcome result = calls.schedule({.userId = 7, .fireAt = 1790000000, .topic = "llamar al dentista", .lang = "es", .commandId = "memory-remind:1:1790000000"});
  CHECK(client->asked == 1);
  return result;
}
}

TEST_CASE("only a confirmed schedule is a scheduled call, and every refusal keeps its reason")
{
  CHECK(answered(NotificationRpcOutcome::Success, grpc::Status::OK) == ReminderCallOutcome::Scheduled);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "SCHEDULE_TOO_FAR")) == ReminderCallOutcome::TooFar);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::OUT_OF_RANGE, "SCHEDULE_IN_PAST")) == ReminderCallOutcome::InThePast);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "too many pending calls")) ==
        ReminderCallOutcome::TooMany);
  CHECK(answered(NotificationRpcOutcome::Unavailable, grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline")) == ReminderCallOutcome::Unavailable);
  CHECK(answered(NotificationRpcOutcome::Unavailable, grpc::Status(grpc::StatusCode::UNAVAILABLE, "down")) == ReminderCallOutcome::Unavailable);
  CHECK(answered(NotificationRpcOutcome::Conflict, grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "command id reused")) == ReminderCallOutcome::Refused);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "topic")) == ReminderCallOutcome::Refused);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "no credential")) == ReminderCallOutcome::Refused);
  CHECK(answered(NotificationRpcOutcome::Rejected, grpc::Status(grpc::StatusCode::INTERNAL, "schedule failed")) == ReminderCallOutcome::Refused);
}

TEST_CASE("with no client the call is not scheduled")
{
  const NotificationReminderCalls calls(nullptr);
  CHECK(calls.schedule({.userId = 7, .fireAt = 1790000000, .topic = "x", .lang = "es", .commandId = "c"}) == ReminderCallOutcome::Unavailable);
}
