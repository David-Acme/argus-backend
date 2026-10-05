#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/guard/guard-risk.hxx>

TEST_CASE("the semantic vocabulary is closed")
{
  RiskEvidence evidence;
  CHECK(guard_risk::evidenceFromTag("concealed_face", evidence));
  CHECK(guard_risk::evidenceFromTag("attempting_door", evidence));
  CHECK(guard_risk::evidenceFromTag("aggressive", evidence));
  CHECK_FALSE(guard_risk::evidenceFromTag("delete_everything", evidence));
  CHECK_FALSE(guard_risk::evidenceFromTag("", evidence));
}

TEST_CASE("bounded evidence raises a soft case")
{
  const GuardRiskResult concealed = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"concealed_face"},
       .hardFloor = false});
  CHECK(concealed.danger == GuardDanger::Medium);
  CHECK(concealed.appliedTags.size() == 1);

  const GuardRiskResult door = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "high",
       .tags = {"attempting_door", "carrying_box"},
       .hardFloor = false});
  CHECK(door.danger == GuardDanger::High);
  CHECK(door.evidenceScore == 3);
}

TEST_CASE("evidence never lowers and never passes a hard floor")
{
  const GuardRiskResult floor = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Critical,
       .threat = "low",
       .tags = {"calm_delivery_reply"},
       .hardFloor = true});
  CHECK(floor.danger == GuardDanger::Critical);

  const GuardRiskResult calm = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "none",
       .tags = {"aggressive", "attempting_door"},
       .hardFloor = false});
  CHECK(calm.danger == GuardDanger::Medium);

  const GuardRiskResult unknown = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "high",
       .tags = {"not_a_real_tag"},
       .hardFloor = false});
  CHECK(unknown.danger == GuardDanger::Medium);
  CHECK(unknown.appliedTags.empty());
}

TEST_CASE("a corroborated weapon is critical, even above a hard floor")
{
  const GuardRiskResult floored = guard_risk::mergeEvidence(
      {.floor = GuardDanger::High,
       .threat = "high",
       .tags = {"knife"},
       .hardFloor = true});
  CHECK(floored.danger == GuardDanger::Critical);
  CHECK(floored.weapon);
  CHECK(floored.appliedTags == std::vector<std::string>{"weapon"});

  const GuardRiskResult guest = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"weapon"},
       .hardFloor = false});
  CHECK(guest.danger == GuardDanger::Critical);

  const GuardRiskResult uncorroborated = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "low",
       .tags = {"weapon"},
       .hardFloor = false});
  CHECK(uncorroborated.danger == GuardDanger::Medium);
  CHECK_FALSE(uncorroborated.weapon);
}

TEST_CASE("a delivery never raises the danger")
{
  const GuardRiskResult delivery = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"carrying_box"},
       .hardFloor = false});
  CHECK(delivery.danger == GuardDanger::Low);
  CHECK(delivery.evidenceScore == 0);
}

TEST_CASE("a repeated tag counts once")
{
  const GuardRiskResult once = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"concealed_face"},
       .hardFloor = false});
  const GuardRiskResult repeated = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"concealed_face", "concealed_face", "concealed_face"},
       .hardFloor = false});
  CHECK(repeated.evidenceScore == once.evidenceScore);
  CHECK(repeated.danger == once.danger);
  CHECK(repeated.appliedTags.size() == 1);
}
