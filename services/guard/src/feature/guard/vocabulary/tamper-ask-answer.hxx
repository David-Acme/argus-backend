#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class TamperAskAnswer : uint8_t
{
  Pending = 0,
  Me,
  No,
  Timeout
};

inline std::string tamperAskAnswerToString(TamperAskAnswer answer)
{
  switch (answer) {
    case TamperAskAnswer::Pending:
      return "";
    case TamperAskAnswer::Me:
      return "me";
    case TamperAskAnswer::No:
      return "no";
    case TamperAskAnswer::Timeout:
      return "timeout";
  }
  return "";
}

inline std::optional<TamperAskAnswer> tamperAskAnswerFromString(std::string_view value)
{
  if (value.empty())
    return TamperAskAnswer::Pending;
  if (value == "me")
    return TamperAskAnswer::Me;
  if (value == "no")
    return TamperAskAnswer::No;
  if (value == "timeout")
    return TamperAskAnswer::Timeout;
  return std::nullopt;
}
