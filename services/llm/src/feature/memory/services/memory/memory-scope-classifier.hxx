#pragma once

#include <shared/vocabulary/memory-scope.hxx>

#include <string_view>

struct MemoryScopeInput
{
  std::string_view text;
  std::string_view lang;
};

[[nodiscard]] bool saysForEveryone(std::string_view text, std::string_view lang);

[[nodiscard]] MemoryScope classifyMemoryScope(const MemoryScopeInput& input);
