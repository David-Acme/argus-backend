#include "notification-agenda-notifier.hxx"

#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

NotificationAgendaNotifier::NotificationAgendaNotifier(
    std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

bool NotificationAgendaNotifier::send(const AgendaNotice& notice) const
{
  if (!client_)
    return false;
  argus::notification::v1::CreateNotificationsRequest request;
  for (const int64_t userId : notice.userIds)
    request.add_user_ids(userId);
  request.set_type("agenda");
  request.set_title(notice.title);
  request.set_body(notice.body);
  request.set_data(json_util::toString(notice.data));
  request.set_command_id(notice.commandId);
  const auto result = client_->createNotifications(
      request,
      {.userId = 0, .role = "system", .device = "argus-productivity"});
  if (result.outcome == NotificationRpcOutcome::Success)
    return true;
  if (result.outcome == NotificationRpcOutcome::Conflict) {
    LOG_WARN << "Agenda: notification command " << notice.commandId
             << " conflicts with an earlier one; recorded as sent";
    return true;
  }
  LOG_WARN << "Agenda: notification " << notice.commandId
           << " not delivered: " << result.status.error_message();
  return false;
}
