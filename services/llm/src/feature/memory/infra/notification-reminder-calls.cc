#include "notification-reminder-calls.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

NotificationReminderCalls::NotificationReminderCalls(
    std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

bool NotificationReminderCalls::schedule(const ReminderCallRequest& request) const
{
  if (!client_)
    return false;
  const auto result = client_->scheduleCall({.userId = request.userId,
                                             .fireAt = request.fireAt,
                                             .topic = request.topic,
                                             .lang = request.lang,
                                             .commandId = request.commandId});
  if (result.outcome == NotificationRpcOutcome::Success)
    return true;
  LOG_WARN << "argus-llm: reminder call " << request.commandId
           << " not scheduled: " << result.status.error_message();
  return false;
}
