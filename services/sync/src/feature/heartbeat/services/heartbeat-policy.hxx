#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>
#include <string_view>

struct HeartbeatFacts
{
  int64_t now{0};
  std::string presence;
  int64_t presenceSince{0};
  int64_t guardSeenAt{0};
};

struct HeartbeatPolicy
{
  int64_t intervalSeconds{60};
  int64_t graceSeconds{2700};
  int64_t socketGraceSeconds{180};
  int64_t guardStaleSeconds{90};
};

namespace heartbeat
{
inline constexpr std::string_view kPresenceHome = "home";
inline constexpr std::string_view kPresenceAway = "away";
inline constexpr std::string_view kPresenceUnknown = "unknown";

[[nodiscard]] std::string normalizePresence(std::string_view presence);

[[nodiscard]] bool armed(std::string_view presence);

[[nodiscard]] std::string_view guardState(const HeartbeatFacts& facts,
                                          const HeartbeatPolicy& policy);

[[nodiscard]] Json::Value render(const HeartbeatFacts& facts,
                                 const HeartbeatPolicy& policy);
}
