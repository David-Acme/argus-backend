#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ResponseState : uint8_t
{
  Active = 0,
  Attended,
  Unanswered,
  Confirmed,
  FalseAlarm,
  Expired
};

inline constexpr std::array<ResponseState, 6> kResponseStates{
    ResponseState::Active,    ResponseState::Attended,   ResponseState::Unanswered,
    ResponseState::Confirmed, ResponseState::FalseAlarm, ResponseState::Expired};

inline std::string responseStateToString(ResponseState state)
{
  switch (state) {
    case ResponseState::Active:
      return "active";
    case ResponseState::Attended:
      return "attended";
    case ResponseState::Unanswered:
      return "unanswered";
    case ResponseState::Confirmed:
      return "confirmed";
    case ResponseState::FalseAlarm:
      return "false_alarm";
    case ResponseState::Expired:
      return "expired";
  }
  return "expired";
}

inline std::optional<ResponseState> responseStateFromString(std::string_view value)
{
  for (const ResponseState state : kResponseStates) {
    if (responseStateToString(state) == value)
      return state;
  }
  return std::nullopt;
}

inline bool responseOpen(ResponseState state)
{
  return state != ResponseState::FalseAlarm && state != ResponseState::Expired;
}

enum class ResponseVerdict : uint8_t
{
  Real = 0,
  FalseAlarm
};

inline std::string responseVerdictToString(ResponseVerdict verdict)
{
  return verdict == ResponseVerdict::Real ? "real" : "false_alarm";
}

inline std::optional<ResponseVerdict> responseVerdictFromString(std::string_view value)
{
  if (value == "real")
    return ResponseVerdict::Real;
  if (value == "false_alarm")
    return ResponseVerdict::FalseAlarm;
  return std::nullopt;
}

enum class ResponseMemberMode : uint8_t
{
  Call = 0,
  Notify
};

inline std::string responseMemberModeToString(ResponseMemberMode mode)
{
  return mode == ResponseMemberMode::Call ? "call" : "notify";
}

inline ResponseMemberMode responseMemberModeFromString(std::string_view value)
{
  return value == "notify" ? ResponseMemberMode::Notify : ResponseMemberMode::Call;
}
