#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

enum class ContextFacet : std::uint8_t
{
  Camera,
  Agenda,
  Guard,
  Reminders,
  Memory,
  Modules,
  Count
};

inline constexpr std::size_t kFacetCount = static_cast<std::size_t>(ContextFacet::Count);

[[nodiscard]] std::string_view contextFacetToString(ContextFacet facet);

[[nodiscard]] ContextFacet contextFacetFromString(std::string_view name);

struct FacetTable
{
  std::string_view language;
  std::array<std::span<const std::string_view>, kFacetCount> words;
  std::array<std::span<const std::string_view>, kFacetCount> phrases;
};

[[nodiscard]] std::span<const FacetTable> facetTables();

[[nodiscard]] std::vector<ContextFacet> facetsInText(std::string_view text, std::string_view lang);

[[nodiscard]] std::optional<ContextFacet> facetForTool(std::string_view tool);

}
