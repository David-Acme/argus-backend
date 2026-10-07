#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace call_time
{

struct Folded
{
  std::string text;
  std::vector<std::size_t> origin;
};

struct Token
{
  std::string text;
  std::size_t begin{0};
  std::size_t end{0};
};

using Tokens = std::vector<Token>;

struct Counted
{
  int value{0};
  std::size_t length{0};
  bool ordinal{false};
};

[[nodiscard]] Folded fold(std::string_view source);

[[nodiscard]] Tokens tokenize(const std::string& text);

[[nodiscard]] bool wordAt(const Tokens& tokens, std::size_t at, std::string_view word);

[[nodiscard]] bool anyWordAt(const Tokens& tokens, std::size_t at, std::initializer_list<std::string_view> words);

[[nodiscard]] int smallNumber(const std::string& token);

[[nodiscard]] std::optional<Counted> dayNumberAt(const Tokens& tokens, std::size_t at);

[[nodiscard]] std::optional<Counted> cardinalDayAt(const Tokens& tokens, std::size_t at);

[[nodiscard]] std::optional<Counted> minutesAt(const Tokens& tokens, std::size_t at);

[[nodiscard]] std::optional<Counted> englishMinutesAt(const Tokens& tokens, std::size_t at);

[[nodiscard]] int monthOf(const std::string& token);

[[nodiscard]] int weekdayOf(const std::string& token);

}
