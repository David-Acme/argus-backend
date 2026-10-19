#pragma once

#include <feature/messenger/services/conversation-port.hxx>
#include <feature/messenger/services/message-courier.hxx>

#include <mutex>
#include <set>
#include <string>
#include <vector>

class FakeCourier final : public messenger::MessageCourier
{
public:
  [[nodiscard]] messenger::CourierReceipt carry(const messenger::CourierLetter& letter) const override
  {
    const std::scoped_lock lock(mutex_);
    letters.push_back(letter);
    if (!accept)
      return {};
    const bool live = speaking.contains(letter.toUserId);
    return {.carried = true,
            .channel = live ? RelayChannel::Conversation : RelayChannel::Notification,
            .messageKey = letter.commandId};
  }

  [[nodiscard]] bool settle(const messenger::CourierSettle& settle) const override
  {
    const std::scoped_lock lock(mutex_);
    settled.push_back(settle);
    return true;
  }

  mutable std::vector<messenger::CourierLetter> letters;
  mutable std::vector<messenger::CourierSettle> settled;
  std::set<int64_t> speaking;
  bool accept{true};

private:
  mutable std::mutex mutex_;
};

class FakeConversation final : public messenger::ConversationPort
{
public:
  [[nodiscard]] bool note(const messenger::ConversationNote& note) const override
  {
    notes.push_back(note);
    return true;
  }

  mutable std::vector<messenger::ConversationNote> notes;
};
