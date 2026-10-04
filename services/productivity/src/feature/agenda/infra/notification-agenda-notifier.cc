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
  const auto result =
      client_->announceAgenda({.userIds = notice.userIds,
                               .leadMinutes = notice.leadMinutes,
                               .title = notice.title,
                               .body = notice.body,
                               .data = json_util::toString(notice.data),
                               .commandId = notice.commandId});
  if (result.outcome == NotificationRpcOutcome::Success)
    return true;
  LOG_WARN << "Agenda: announcement " << notice.commandId
           << " not delivered: " << result.status.error_message();
  return false;
}
