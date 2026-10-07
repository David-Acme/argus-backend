#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

enum class TapoControlState : uint8_t
{
  Idle = 0,
  Ready,
  LockedOut,
  Refused,
  Unreachable,
  NoCredentials,
  NotApplicable
};

inline std::string tapoControlStateToString(TapoControlState state)
{
  switch (state) {
    case TapoControlState::Idle:
      return "idle";
    case TapoControlState::Ready:
      return "ready";
    case TapoControlState::LockedOut:
      return "locked_out";
    case TapoControlState::Refused:
      return "refused";
    case TapoControlState::Unreachable:
      return "unreachable";
    case TapoControlState::NoCredentials:
      return "no_credentials";
    case TapoControlState::NotApplicable:
      return "not_applicable";
  }
  return "idle";
}

struct TapoControlStatus
{
  TapoControlState state{TapoControlState::Idle};
  std::string credential;
  int64_t retryAtMs{0};
  int code{0};
  std::string message;
};

namespace tapo_control
{
[[nodiscard]] int retryAfterSeconds(const TapoControlStatus& status, int64_t nowMs);
[[nodiscard]] Json::Value toJson(const TapoControlStatus& status, int64_t nowMs);
[[nodiscard]] int64_t systemNowMs();
}
