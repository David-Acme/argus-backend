#pragma once

#include <json/value.h>
#include <optional>

namespace tapo_lockout
{
inline constexpr int kLockedOutCode = -40404;
inline constexpr int kUnknownLockoutSeconds = 60;

struct Reading
{
  int secLeft{0};
  int code{0};
};

[[nodiscard]] std::optional<Reading> of(const Json::Value& response);
}
