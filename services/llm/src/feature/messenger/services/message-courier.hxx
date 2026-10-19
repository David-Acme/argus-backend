#pragma once

#include <feature/messenger/vocabulary/relay-outcome.hxx>

#include <cstdint>
#include <string>

namespace messenger
{

enum class CourierKind : std::uint8_t
{
  Relay,
  Reply,
  Visitor
};

struct CourierLetter
{
  int64_t fromUserId{0};
  std::string fromName;
  int64_t toUserId{0};
  std::string toName;
  CourierKind kind{CourierKind::Relay};
  std::string body;
  std::string spokenLine;
  std::string title;
  std::string replyToKey;
  std::string commandId;
  int64_t expiresAt{0};
};

struct CourierReceipt
{
  bool carried{false};
  RelayChannel channel{RelayChannel::Notification};
  std::string messageKey;
};

enum class CourierSettlement : std::uint8_t
{
  Answered,
  Expired
};

struct CourierSettle
{
  std::string messageKey;
  CourierSettlement settlement{CourierSettlement::Expired};
};

class MessageCourier
{
public:
  MessageCourier() = default;
  virtual ~MessageCourier() = default;
  MessageCourier(const MessageCourier&) = delete;
  MessageCourier& operator=(const MessageCourier&) = delete;

  [[nodiscard]] virtual CourierReceipt carry(const CourierLetter& letter) const = 0;
  [[nodiscard]] virtual bool settle(const CourierSettle& settle) const = 0;
};

}
