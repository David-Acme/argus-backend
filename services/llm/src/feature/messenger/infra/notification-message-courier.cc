#include "notification-message-courier.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
namespace v1 = argus::notification::v1;

v1::MessageKind wireKind(messenger::CourierKind kind)
{
  switch (kind) {
  case messenger::CourierKind::Relay: return v1::MESSAGE_KIND_RELAY;
  case messenger::CourierKind::Reply: return v1::MESSAGE_KIND_REPLY;
  case messenger::CourierKind::Visitor: return v1::MESSAGE_KIND_VISITOR;
  }
  return v1::MESSAGE_KIND_UNSPECIFIED;
}
}

v1::RelayMessageRequest relayRequestOf(const messenger::CourierLetter& letter)
{
  v1::RelayMessageRequest request;
  request.set_from_user_id(letter.fromUserId);
  request.set_from_name(letter.fromName);
  request.set_to_user_id(letter.toUserId);
  request.set_to_name(letter.toName);
  request.set_kind(wireKind(letter.kind));
  request.set_body(letter.body);
  request.set_spoken_line(letter.spokenLine);
  request.set_title(letter.title);
  request.set_reply_to_key(letter.replyToKey);
  request.set_command_id(letter.commandId);
  request.set_expires_at(letter.expiresAt);
  return request;
}

NotificationMessageCourier::NotificationMessageCourier(std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

messenger::CourierReceipt NotificationMessageCourier::carry(const messenger::CourierLetter& letter) const
{
  if (!client_)
    return {};
  const NotificationRelayResult result = client_->relayMessage(relayRequestOf(letter));
  if (result.outcome != NotificationRpcOutcome::Success) {
    LOG_WARN << "argus-llm: argus-notification did not take a relayed message: " << result.status.error_message();
    return {};
  }
  return {.carried = true,
          .channel = result.response.channel() == v1::MESSAGE_CHANNEL_CONVERSATION ? RelayChannel::Conversation
                                                                                  : RelayChannel::Notification,
          .messageKey = result.response.message_key()};
}

bool NotificationMessageCourier::settle(const messenger::CourierSettle& settle) const
{
  if (!client_)
    return false;
  v1::SettleMessageRequest request;
  request.set_message_key(settle.messageKey);
  request.set_settlement(settle.settlement == messenger::CourierSettlement::Answered ? v1::MESSAGE_SETTLEMENT_ANSWERED
                                                                                    : v1::MESSAGE_SETTLEMENT_EXPIRED);
  return client_->settleMessage(request) == NotificationRpcOutcome::Success;
}
