#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MemoryScope : uint8_t
{
  Personal = 0,
  Household
};

inline std::string memoryScopeToString(MemoryScope scope)
{
  switch (scope) {
    case MemoryScope::Personal:
      return "user";
    case MemoryScope::Household:
      return "household";
  }
  return {};
}

inline std::optional<MemoryScope> memoryScopeFromString(std::string_view name)
{
  if (name == "user")
    return MemoryScope::Personal;
  if (name == "household")
    return MemoryScope::Household;
  return std::nullopt;
}
