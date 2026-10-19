#pragma once

#include <feature/conversation/services/conversation-service.hxx>
#include <feature/messenger/services/conversation-port.hxx>

#include <memory>

class ConversationInbound final : public messenger::ConversationPort
{
public:
  explicit ConversationInbound(std::shared_ptr<const ConversationService> conversations);

  [[nodiscard]] bool note(const messenger::ConversationNote& note) const override;

private:
  std::shared_ptr<const ConversationService> conversations_;
};
