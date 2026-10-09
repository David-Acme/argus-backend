#pragma once

#include <span>
#include <string_view>

namespace turn::speech
{

struct CueRow
{
  std::string_view slot;
  std::string_view language;
  std::span<const std::string_view> cues;
};

struct CueQuery
{
  std::string_view slot;
  std::string_view lang;
};

struct ActionQuery
{
  std::string_view tool;
  std::string_view lang;
};

[[nodiscard]] std::span<const CueRow> cueRows();

[[nodiscard]] std::span<const std::string_view> cuesFor(const CueQuery& query);

[[nodiscard]] std::span<const std::string_view> actionMarkers(const ActionQuery& query);

[[nodiscard]] std::string_view slotActionName(const ActionQuery& query);

}
