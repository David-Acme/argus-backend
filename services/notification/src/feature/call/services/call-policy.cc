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

bool call_policy::inQuietHours(const QuietWindowInput& input)
{
  const int start = input.startHour;
  const int end = input.endHour;
  if (start < 0 || end < 0 || start > 23 || end > 23 || start == end)
    return false;
  bool inside = false;
  int windowDay = input.weekday;
  if (start < end) {
    inside = input.hour >= start && input.hour < end;
  }
  else if (input.hour >= start) {
    inside = true;
  }
  else if (input.hour < end) {
    inside = true;
    windowDay = (input.weekday + 6) % 7;
  }
  return inside && ((static_cast<unsigned>(input.days) >>
                     static_cast<unsigned>(windowDay)) & 1U) != 0;
}

namespace
{
CallVerdict limited(const CallPolicyInput& input, const char* ringReason)
{
  const bool live = input.preference.liveAnnounce;
  if (input.ringing)
    return {.decision = CallDecision::Followup, .reason = "ringing", .injectable = live};
  if (!input.critical) {
    if (input.lastCallAt > 0 && input.now - input.lastCallAt < input.limits.callGapS)
      return {.decision = CallDecision::Notify, .reason = "cooldown", .injectable = live};
    if (input.callsLastHour >= input.limits.maxCallsPerHour)
      return {.decision = CallDecision::Notify, .reason = "hourly_cap", .injectable = live};
  }
  return {.decision = CallDecision::Ring, .reason = ringReason, .injectable = live};
}
}

CallVerdict call_policy::decide(const CallPolicyInput& input)
{
  const CallPreferenceSchema& preference = input.preference;
  if (input.alreadyCalled)
    return {.decision = CallDecision::Drop,
            .reason = "already_called",
            .injectable = false};

  if (input.mandatory) {
    if (!input.limits.enabled)
      return {.decision = CallDecision::Notify,
              .reason = "calls_disabled",
              .injectable = false};
    return limited(input, "on_duty");
  }

  const CallMode mode = preference.modeFor(input.trigger);
  if (mode == CallMode::Off)
    return {.decision = CallDecision::Drop,
            .reason = "trigger_off",
            .injectable = false};
  if (mode == CallMode::Notify)
    return {.decision = CallDecision::Notify,
            .reason = "trigger_notify",
            .injectable = false};
  if (input.planNotify)
    return {.decision = CallDecision::Notify,
            .reason = "plan_notify",
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
            .injectable = preference.liveAnnounce};
  if (inQuietHours({.hour = input.localHour,
                    .weekday = input.localWeekday,
                    .startHour = preference.quietStartHour,
                    .endHour = preference.quietEndHour,
                    .days = preference.quietDays}) &&
      !bypass)
    return {.decision = CallDecision::Notify,
            .reason = "quiet_hours",
            .injectable = preference.liveAnnounce};

  return limited(input, "ring");
}
