#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

enum class PersonCategory : std::uint8_t
{
  None = 0,
  Neighbor,
  Delivery,
  Service,
  Family,
  Acquaintance,
  Watchlist
};

inline constexpr std::array<std::string_view, 7> kPersonCategoryNames = {
    "", "neighbor", "delivery", "service", "family", "acquaintance", "watchlist"};

[[nodiscard]] constexpr std::string_view personCategoryToString(
    PersonCategory category)
{
  return kPersonCategoryNames.at(static_cast<std::size_t>(category));
}

[[nodiscard]] constexpr std::optional<PersonCategory> personCategoryFromString(
    std::string_view value)
{
  for (std::size_t i = 0; i < kPersonCategoryNames.size(); ++i)
    if (kPersonCategoryNames.at(i) == value)
      return static_cast<PersonCategory>(i);
  return std::nullopt;
}

[[nodiscard]] constexpr bool personCategoryLowersRisk(PersonCategory category)
{
  return category != PersonCategory::None &&
         category != PersonCategory::Watchlist;
}
