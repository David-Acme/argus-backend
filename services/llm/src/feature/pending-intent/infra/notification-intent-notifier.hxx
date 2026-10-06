#pragma once

#include <feature/pending-intent/services/intent-notifier.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationIntentNotifier final : public IntentNotifier
{
public:
  explicit NotificationIntentNotifier(std::shared_ptr<const NotificationClient> client);

  [[nodiscard]] bool tell(const IntentNotice& notice) const override;

private:
  std::shared_ptr<const NotificationClient> client_;
};
