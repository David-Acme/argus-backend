#include "notification-reminder-calls.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

NotificationReminderCalls::NotificationReminderCalls(
    std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

namespace
{
ReminderCallOutcome refusalOf(const NotificationCallScheduleResult& result)
{
  if (result.outcome == NotificationRpcOutcome::Unavailable)
    return ReminderCallOutcome::Unavailable;
  const std::string& message = result.status.error_message();
  if (message.find("SCHEDULE_TOO_FAR") != std::string::npos)
    return ReminderCallOutcome::TooFar;
  if (message.find("SCHEDULE_IN_PAST") != std::string::npos)
    return ReminderCallOutcome::InThePast;
  if (message.find("too many pending calls") != std::string::npos)
    return ReminderCallOutcome::TooMany;
  return ReminderCallOutcome::Refused;
}
}

ReminderCallOutcome NotificationReminderCalls::schedule(const ReminderCallRequest& request) const
{
  if (!client_)
    return ReminderCallOutcome::Unavailable;
  const auto result = client_->scheduleCall({.userId = request.userId,
                                             .fireAt = request.fireAt,
                                             .topic = request.topic,
                                             .lang = request.lang,
                                             .commandId = request.commandId});
  if (result.outcome == NotificationRpcOutcome::Success)
    return ReminderCallOutcome::Scheduled;
  LOG_WARN << "argus-llm: reminder call " << request.commandId
           << " not scheduled: " << result.status.error_message();
  return refusalOf(result);
}
