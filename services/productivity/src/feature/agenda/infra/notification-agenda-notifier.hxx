#pragma once

#include <feature/agenda/services/agenda-announcer.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationAgendaNotifier final : public AgendaNotifier
{
public:
  explicit NotificationAgendaNotifier(std::shared_ptr<const NotificationClient> client);

  [[nodiscard]] bool send(const AgendaNotice& notice) const override;

private:
  std::shared_ptr<const NotificationClient> client_;
};
