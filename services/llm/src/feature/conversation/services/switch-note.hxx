#pragma once

#include <llm/conversation-leg.hxx>

#include <string>
#include <string_view>

struct SwitchNoteInput
{
  std::string_view lang;
  ConversationLegKind from{ConversationLegKind::Camera};
  ConversationLegKind to{ConversationLegKind::Phone};
  std::string_view cameraPlaceWithArticle;
};

[[nodiscard]] std::string switchNote(const SwitchNoteInput& input);
