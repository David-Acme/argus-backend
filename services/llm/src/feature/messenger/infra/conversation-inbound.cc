#include "conversation-inbound.hxx"

#include <utility>

ConversationInbound::ConversationInbound(std::shared_ptr<const ConversationService> conversations)
    : conversations_(std::move(conversations))
{
}

bool ConversationInbound::note(const messenger::ConversationNote& note) const
{
  if (!conversations_)
    return false;
  return conversations_->appendInbound({.userId = note.userId, .content = note.text, .at = note.at});
}
