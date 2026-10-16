#pragma once

#include <cstdint>
#include <string_view>

enum class TurnCoherence : uint8_t
{
  Proceed,
  AskAddressed,
  Ignore
};

[[nodiscard]] bool isAddressed(std::string_view transcript);

[[nodiscard]] bool hasContentWords(std::string_view transcript);

[[nodiscard]] TurnCoherence turnCoherenceOf(std::string_view transcript);
