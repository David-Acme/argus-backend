#include "presence-signal.hxx"

bool PresenceSignalThrottle::informative(SessionOrigin origin)
{
  return origin == SessionOrigin::Lan || origin == SessionOrigin::Tunnel;
}

bool PresenceSignalThrottle::admit(const PresenceThrottleInput& input)
{
  if (!informative(input.origin) || input.sessionId.empty())
    return false;
  const std::scoped_lock lock(mutex_);
  const auto known = lastOrigin_.find(std::string(input.sessionId));
  const bool changed =
      known == lastOrigin_.end() || known->second != input.origin;
  if (!changed && !input.seenAdvanced)
    return false;
  if (known != lastOrigin_.end()) {
    known->second = input.origin;
    return true;
  }
  if (lastOrigin_.size() >= kMaxSessions)
    lastOrigin_.clear();
  lastOrigin_.emplace(std::string(input.sessionId), input.origin);
  return true;
}
