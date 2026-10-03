#pragma once

#include <cstdint>
#include <string>

enum class SpanishGender : std::uint8_t
{
  Standalone,
  Masculine,
  Feminine
};

struct SpanishCardinalInput
{
  std::uint64_t value{0};
  SpanishGender gender{SpanishGender::Standalone};
};

[[nodiscard]] std::string spanishCardinal(const SpanishCardinalInput& input);

struct SpanishOrdinalInput
{
  std::uint64_t value{0};
  bool feminine{false};
  bool apocope{false};
};

[[nodiscard]] std::string spanishOrdinal(const SpanishOrdinalInput& input);

[[nodiscard]] std::string englishCardinal(std::uint64_t value);
[[nodiscard]] std::string englishOrdinal(std::uint64_t value);
[[nodiscard]] std::string englishYear(std::uint64_t value);

[[nodiscard]] std::string spanishMonth(int month);
[[nodiscard]] std::string englishMonth(int month);
