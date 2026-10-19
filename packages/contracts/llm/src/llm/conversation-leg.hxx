#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ConversationLegKind : uint8_t
{
  Camera = 0,
  Phone,
  Laptop
};

inline std::string conversationLegKindToString(ConversationLegKind kind)
{
  switch (kind) {
    case ConversationLegKind::Camera:
      return "camera";
    case ConversationLegKind::Phone:
      return "phone";
    case ConversationLegKind::Laptop:
      return "laptop";
  }
  return {};
}

inline std::optional<ConversationLegKind> conversationLegKindFromString(std::string_view name)
{
  if (name == "camera")
    return ConversationLegKind::Camera;
  if (name == "phone")
    return ConversationLegKind::Phone;
  if (name == "laptop")
    return ConversationLegKind::Laptop;
  return std::nullopt;
}
