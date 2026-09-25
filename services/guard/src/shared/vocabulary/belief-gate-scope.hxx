#pragma once

#include <optional>
#include <string>

enum class BeliefGateScope
{
  Notify,
  Communication,
  All,
};

inline std::optional<BeliefGateScope>
beliefGateScopeFromString(const std::string& value)
{
  if (value == "notify")
    return BeliefGateScope::Notify;
  if (value == "communication")
    return BeliefGateScope::Communication;
  if (value == "all")
    return BeliefGateScope::All;
  return std::nullopt;
}

inline std::string beliefGateScopeToString(BeliefGateScope scope)
{
  switch (scope) {
  case BeliefGateScope::Notify:
    return "notify";
  case BeliefGateScope::Communication:
    return "communication";
  case BeliefGateScope::All:
    return "all";
  }
  return "notify";
}
