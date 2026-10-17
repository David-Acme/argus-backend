#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/guard/guard-risk.hxx>
#include <feature/guard/guard-weapon-lexicon.hxx>

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
       .caption = {},
       .hardFloor = false});
  CHECK(concealed.danger == GuardDanger::Medium);
  CHECK(concealed.appliedTags.size() == 1);

  const GuardRiskResult door = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "high",
       .tags = {"attempting_door", "carrying_box"},
       .caption = {},
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
       .caption = {},
       .hardFloor = true});
  CHECK(floor.danger == GuardDanger::Critical);

  const GuardRiskResult calm = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "none",
       .tags = {"aggressive", "attempting_door"},
       .caption = {},
       .hardFloor = false});
  CHECK(calm.danger == GuardDanger::Medium);

  const GuardRiskResult unknown = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "high",
       .tags = {"not_a_real_tag"},
       .caption = {},
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
       .caption = {},
       .hardFloor = true});
  CHECK(floored.danger == GuardDanger::Critical);
  CHECK(floored.weapon);
  CHECK(floored.appliedTags == std::vector<std::string>{"weapon"});

  const GuardRiskResult guest = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"weapon"},
       .caption = {},
       .hardFloor = false});
  CHECK(guest.danger == GuardDanger::Critical);

  const GuardRiskResult uncorroborated = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "low",
       .tags = {"weapon"},
       .caption = {},
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
       .caption = {},
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
       .caption = {},
       .hardFloor = false});
  const GuardRiskResult repeated = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"concealed_face", "concealed_face", "concealed_face"},
       .caption = {},
       .hardFloor = false});
  CHECK(repeated.evidenceScore == once.evidenceScore);
  CHECK(repeated.danger == once.danger);
  CHECK(repeated.appliedTags.size() == 1);
}

TEST_CASE("a weapon or a high threat revokes an expected guest's pass")
{
  CHECK(guard_risk::revokesGuestPass({.threat = "none", .tags = {"weapon"}}));
  CHECK(guard_risk::revokesGuestPass({.threat = "low", .tags = {"knife"}}));
  CHECK(guard_risk::revokesGuestPass({.threat = "none", .tags = {"raised_object"}}));
  CHECK(guard_risk::revokesGuestPass({.threat = "high", .tags = {}}));
  CHECK_FALSE(guard_risk::revokesGuestPass({.threat = "medium", .tags = {"loitering"}}));
  CHECK_FALSE(guard_risk::revokesGuestPass({.threat = "none", .tags = {"carrying_box"}}));
}

TEST_CASE("the caption lexicon asserts a person-held weapon in both languages")
{
  CHECK(guard_weapon::captionAssertsWeapon(
      "The person is holding a knife in their right hand."));
  CHECK(guard_weapon::captionAssertsWeapon("The man carries a machete."));
  CHECK(guard_weapon::captionAssertsWeapon("They hold a gun in both hands."));
  CHECK(guard_weapon::captionAssertsWeapon(
      "A small blade in his hand, pointed at the camera."));
  CHECK(guard_weapon::captionAssertsWeapon(
      "The person is cutting a cake with a knife."));
  CHECK(guard_weapon::captionAssertsWeapon(
      "The individual carries knives in their hands."));
  CHECK(guard_weapon::captionAssertsWeapon(
      "La persona sostiene un cuchillo."));
  CHECK(guard_weapon::captionAssertsWeapon("Lleva una pistola en la mano."));
  CHECK(guard_weapon::captionAssertsWeapon("El hombre empuña un revólver."));
  CHECK(guard_weapon::captionAssertsWeapon("Sujeta una navaja con la mano."));
  CHECK(guard_weapon::captionAssertsWeapon("Se ve un rifle en sus manos."));
}

TEST_CASE("negation, resemblance and background naming never assert")
{
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "There is no visible knife in their hands."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "They are not carrying a gun."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "The person is without a knife in his hand."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon("He never holds a machete."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon("No se ve ningún cuchillo."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon("Sin arma."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "No lleva ningún cuchillo en la mano."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "Tampoco sostiene una pistola."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon("Ni pistola ni cuchillo."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "carrying a small object in their right hand that resembles a knife"));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon("a knife-like object"));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "various items like knives and utensils"));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "There is a knife on the table."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "Los cuchillos en el bloque de la cocina."));
  CHECK_FALSE(guard_weapon::captionAssertsWeapon(
      "The kitchen counter shows knives and a chopping board."));
}

TEST_CASE("a caption weapon reaches critical beside the tag path")
{
  const GuardRiskResult tagOnly = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {"knife"},
       .caption = {},
       .hardFloor = false});
  CHECK(tagOnly.danger == GuardDanger::Critical);
  CHECK(tagOnly.weapon);

  const GuardRiskResult captionOnly = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "medium",
       .tags = {},
       .caption = "The person is holding a knife in their right hand.",
       .hardFloor = false});
  CHECK(captionOnly.danger == GuardDanger::Critical);
  CHECK(captionOnly.weapon);
  CHECK(captionOnly.appliedTags == std::vector<std::string>{"weapon"});

  const GuardRiskResult both = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Low,
       .threat = "high",
       .tags = {"weapon"},
       .caption = "Lleva un cuchillo en la mano.",
       .hardFloor = false});
  CHECK(both.danger == GuardDanger::Critical);
  CHECK(both.appliedTags == std::vector<std::string>{"weapon"});

  const GuardRiskResult quiet = guard_risk::mergeEvidence(
      {.floor = GuardDanger::Medium,
       .threat = "high",
       .tags = {},
       .caption = "There is no visible knife in their hands.",
       .hardFloor = false});
  CHECK(quiet.danger == GuardDanger::Medium);
  CHECK_FALSE(quiet.weapon);
  CHECK(quiet.appliedTags.empty());
}

TEST_CASE("a caption weapon only turns critical above a low threat")
{
  for (const char* threat : {"none", "low"}) {
    const GuardRiskResult risk = guard_risk::mergeEvidence(
        {.floor = GuardDanger::Medium,
         .threat = threat,
         .tags = {},
         .caption = "The man holds a machete.",
         .hardFloor = false});
    CHECK(risk.danger == GuardDanger::Medium);
    CHECK_FALSE(risk.weapon);
  }
  for (const char* threat : {"medium", "high", "critical"}) {
    const GuardRiskResult risk = guard_risk::mergeEvidence(
        {.floor = GuardDanger::Medium,
         .threat = threat,
         .tags = {},
         .caption = "The man holds a machete.",
         .hardFloor = false});
    CHECK(risk.danger == GuardDanger::Critical);
    CHECK(risk.weapon);
  }
}
