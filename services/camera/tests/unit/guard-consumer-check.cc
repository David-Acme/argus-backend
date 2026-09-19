#include <doctest/doctest.h>
#include <guard-policy.hxx>

void checkGuardKnownEvent(const Json::Value& event)
{
  const auto signals = guard_policy::parseObjectEvent(event);
  CHECK(signals.hasKnown);
  CHECK_FALSE(signals.hasUnknown);
  CHECK(signals.personId == 7);
  CHECK(signals.knownPersonId == 7);
  CHECK(signals.identityState == IdentityState::Known);
  GuardContext context;
  context.mode = GuardMode::Away;
  context.rule = signals.rule;
  context.severity = signals.severity;
  context.hasKnown = signals.hasKnown;
  context.hasUnknown = signals.hasUnknown;
  context.inAlertZone = true;
  CHECK(guard_policy::evaluate(context) == GuardDanger::None);

  auto inconsistent = event;
  inconsistent["objects"][0]["identityState"] = "unobservable";
  const auto rejected = guard_policy::parseObjectEvent(inconsistent);
  CHECK_FALSE(rejected.hasKnown);
  CHECK(rejected.hasUnknown);
  context.hasKnown = rejected.hasKnown;
  context.hasUnknown = rejected.hasUnknown;
  CHECK(guard_policy::evaluate(context) == GuardDanger::Critical);
}
