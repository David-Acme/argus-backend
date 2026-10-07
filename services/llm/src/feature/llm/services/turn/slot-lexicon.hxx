#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace slot_lexicon
{

enum class Group : std::uint8_t
{
  Clitic,
  Determiner,
  Connector,
  Preposition,
  TimeAnchor,
  TimeFiller,
  Naming,
  AnswerPrefix,
  Modal,
  Other,
  NewOne,
  Count
};

inline constexpr std::size_t kGroupCount = static_cast<std::size_t>(Group::Count);

struct Table
{
  std::string_view language;
  std::array<std::span<const std::string_view>, kGroupCount> groups;
};

[[nodiscard]] std::span<const Table> tables();

}
