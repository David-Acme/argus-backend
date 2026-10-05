#include "heartbeat-policy.hxx"

std::string heartbeat::normalizePresence(std::string_view presence)
{
  if (presence == kPresenceHome || presence == kPresenceAway)
    return std::string(presence);
  return std::string(kPresenceUnknown);
}

bool heartbeat::armed(std::string_view presence)
{
  return presence == kPresenceAway;
}

std::string_view heartbeat::guardState(const HeartbeatFacts& facts,
                                  const HeartbeatPolicy& policy)
{
  if (facts.guardSeenAt <= 0)
    return "unknown";
  return facts.now - facts.guardSeenAt > policy.guardStaleSeconds ? "stale"
                                                                  : "alive";
}

Json::Value heartbeat::render(const HeartbeatFacts& facts,
                              const HeartbeatPolicy& policy)
{
  const std::string presence = normalizePresence(facts.presence);
  Json::Value info(Json::objectValue);
  info["at"] = static_cast<Json::Int64>(facts.now);
  info["intervalSeconds"] = static_cast<Json::Int64>(policy.intervalSeconds);
  info["graceSeconds"] = static_cast<Json::Int64>(policy.graceSeconds);
  info["socketGraceSeconds"] = static_cast<Json::Int64>(policy.socketGraceSeconds);
  info["armed"] = armed(presence);
  info["presence"] = presence;
  info["presenceSince"] =
      static_cast<Json::Int64>(presence == kPresenceUnknown ? 0 : facts.presenceSince);
  info["guard"] = std::string(guardState(facts, policy));
  info["guardSeenAt"] = static_cast<Json::Int64>(facts.guardSeenAt);
  return info;
}
