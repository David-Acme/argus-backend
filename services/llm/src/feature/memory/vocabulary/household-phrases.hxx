#pragma once

#include <array>
#include <string_view>

namespace memory_vocabulary
{

struct HouseholdCue
{
  std::string_view phrase;
  bool mustEnd;
};

inline constexpr std::array<HouseholdCue, 4> kHouseholdCuesEs{{
    {.phrase = "esto es para todos", .mustEnd = true},
    {.phrase = "que lo sepan todos", .mustEnd = false},
    {.phrase = "dile a todos", .mustEnd = false},
    {.phrase = "para toda la casa", .mustEnd = false},
}};

inline constexpr std::array<HouseholdCue, 3> kHouseholdCuesEn{{
    {.phrase = "this is for everyone", .mustEnd = true},
    {.phrase = "let everyone know", .mustEnd = false},
    {.phrase = "for the whole house", .mustEnd = false},
}};

}
