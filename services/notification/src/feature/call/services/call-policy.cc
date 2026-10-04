#include "call-policy.hxx"

std::string callDecisionToString(CallDecision decision)
{
  switch (decision) {
    case CallDecision::Ring:
      return "ring";
    case CallDecision::Followup:
      return "followup";
    case CallDecision::Notify:
      return "notify";
    case CallDecision::Drop:
      return "drop";
  }
  return "notify";
}

bool call_policy::inQuietHours(int hour, int startHour, int endHour)
{
  if (startHour < 0 || endHour < 0 || startHour > 23 || endHour > 23 ||
      startHour == endHour)
    return false;
  if (startHour < endHour)
    return hour >= startHour && hour < endHour;
  return hour >= startHour || hour < endHour;
}

CallVerdict call_policy::decide(const CallPolicyInput& input)
{
  const CallPreferenceSchema& preference = input.preference;
  if (input.alreadyCalled)
    return {.decision = CallDecision::Drop,
            .reason = "already_called",
            .injectable = false};

  const CallMode mode = preference.modeFor(input.trigger);
  if (mode == CallMode::Off)
    return {.decision = CallDecision::Drop,
            .reason = "trigger_off",
            .injectable = false};
  if (mode == CallMode::Notify)
    return {.decision = CallDecision::Notify,
            .reason = "trigger_notify",
            .injectable = false};
  if (!input.limits.enabled)
    return {.decision = CallDecision::Notify,
            .reason = "calls_disabled",
            .injectable = false};
  if (!preference.enabled)
    return {.decision = CallDecision::Notify,
            .reason = "calls_off",
            .injectable = false};
  if (isGuardTrigger(input.trigger) && preference.mutes(input.environmentId))
    return {.decision = CallDecision::Notify,
            .reason = "environment_muted",
            .injectable = false};

  const bool bypass = input.critical && preference.criticalBypass;
  if (preference.dndUntil > input.now && !bypass)
    return {.decision = CallDecision::Notify,
            .reason = "do_not_disturb",
            .injectable = true};
  if (inQuietHours(input.localHour, preference.quietStartHour,
                   preference.quietEndHour) &&
      !bypass)
    return {.decision = CallDecision::Notify,
            .reason = "quiet_hours",
            .injectable = true};

  if (input.ringing)
    return {.decision = CallDecision::Followup,
            .reason = "ringing",
            .injectable = true};

  if (!input.critical) {
    if (input.lastCallAt > 0 &&
        input.now - input.lastCallAt < input.limits.callGapS)
      return {.decision = CallDecision::Notify,
              .reason = "cooldown",
              .injectable = true};
    if (input.callsLastHour >= input.limits.maxCallsPerHour)
      return {.decision = CallDecision::Notify,
              .reason = "hourly_cap",
              .injectable = true};
  }

  return {.decision = CallDecision::Ring, .reason = "ring", .injectable = true};
}
