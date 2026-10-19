#pragma once

#include <feature/messenger/services/message-courier.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationMessageCourier final : public messenger::MessageCourier
{
public:
  explicit NotificationMessageCourier(std::shared_ptr<const NotificationClient> client);

  [[nodiscard]] messenger::CourierReceipt carry(const messenger::CourierLetter& letter) const override;
  [[nodiscard]] bool settle(const messenger::CourierSettle& settle) const override;

private:
  std::shared_ptr<const NotificationClient> client_;
};

[[nodiscard]] argus::notification::v1::RelayMessageRequest relayRequestOf(const messenger::CourierLetter& letter);
