#include "guard-action.hxx"

#include <utility>

GuardActionAuthorizer::GuardActionAuthorizer(Config config)
    : config_(std::move(config))
{
}

GuardActionDecision
GuardActionAuthorizer::authorize(const GuardActionRequest& request) const
{
  const int rank = guardDangerRank(request.danger);
  switch (request.kind) {
    case GuardActionKind::Notify:
      if (rank < config_.notifyLevel)
        return {.authorized = false, .reason = "below_notify_level"};
      return {.authorized = true, .reason = "notify_level"};
    case GuardActionKind::Announce:
      if (rank < config_.announceLevel)
        return {.authorized = false, .reason = "below_announce_level"};
      return {.authorized = true, .reason = "announce_level"};
    case GuardActionKind::Alarm:
      if (rank < config_.alarmLevel)
        return {.authorized = false, .reason = "below_alarm_level"};
      return {.authorized = true, .reason = "alarm_level"};
    case GuardActionKind::SirenArm:
      if (!config_.armSiren)
        return {.authorized = false, .reason = "siren_disabled"};
      if (rank < config_.alarmLevel)
        return {.authorized = false, .reason = "below_alarm_level"};
      return {.authorized = true, .reason = "siren_lease"};
    case GuardActionKind::SirenDisarm:
      return {.authorized = true, .reason = "safety_disarm"};
    case GuardActionKind::Greet:
      if (!config_.dialogueEnabled || !request.greetingEnabled)
        return {.authorized = false, .reason = "dialogue_disabled"};
      if (guardDangerRank(request.danger) >=
          static_cast<int>(GuardDanger::High))
        return {.authorized = false, .reason = "hard_floor"};
      return {.authorized = true, .reason = "dialogue"};
    case GuardActionKind::Reply:
      if (!config_.dialogueEnabled || !request.replyRequested)
        return {.authorized = false, .reason = "dialogue_disabled"};
      if (guardDangerRank(request.danger) >=
          static_cast<int>(GuardDanger::High))
        return {.authorized = false, .reason = "hard_floor"};
      return {.authorized = true, .reason = "dialogue"};
    case GuardActionKind::Listen:
      if (!config_.dialogueEnabled)
        return {.authorized = false, .reason = "dialogue_disabled"};
      return {.authorized = true, .reason = "dialogue"};
  }
  return {.authorized = false, .reason = "unknown_action"};
}
