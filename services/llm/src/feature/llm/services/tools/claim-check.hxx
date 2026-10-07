#pragma once

#include <string>

struct TurnState
{
  bool asked{false};
  bool appAsked{false};
  bool wrote{false};
  bool opened{false};
  bool called{false};
  std::string lang{};
};

[[nodiscard]] bool claimedWithoutTool(const std::string& reply, const TurnState& state);
