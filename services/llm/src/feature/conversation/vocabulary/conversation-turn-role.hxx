#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ConversationTurnRole : uint8_t
{
  User,
  Assistant,
  Note
};

inline std::string conversationTurnRoleToString(ConversationTurnRole role)
{
  switch (role) {
    case ConversationTurnRole::User:
      return "user";
    case ConversationTurnRole::Assistant:
      return "assistant";
    case ConversationTurnRole::Note:
      return "note";
  }
  return {};
}

inline std::optional<ConversationTurnRole> conversationTurnRoleFromString(std::string_view name)
{
  if (name == "user")
    return ConversationTurnRole::User;
  if (name == "assistant")
    return ConversationTurnRole::Assistant;
  if (name == "note")
    return ConversationTurnRole::Note;
  return std::nullopt;
}
