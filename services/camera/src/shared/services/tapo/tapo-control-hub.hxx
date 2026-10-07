#pragma once

#include <cstdint>
#include <optional>
#include <shared/services/tapo/tapo-control-status.hxx>
#include <string>

namespace tapo_control_hub
{
struct Hold
{
  TapoControlState state{TapoControlState::LockedOut};
  int64_t untilMs{0};
  int code{0};
};

[[nodiscard]] std::optional<Hold> holdFor(const std::string& endpoint, int64_t nowMs);
void hold(const std::string& endpoint, const Hold& hold);
void release(const std::string& endpoint);
void reset();
}
