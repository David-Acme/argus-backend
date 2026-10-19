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
    {"esto es para todos", true},
    {"que lo sepan todos", false},
    {"dile a todos", false},
    {"para toda la casa", false},
}};

inline constexpr std::array<HouseholdCue, 3> kHouseholdCuesEn{{
    {"this is for everyone", true},
    {"let everyone know", false},
    {"for the whole house", false},
}};

}
