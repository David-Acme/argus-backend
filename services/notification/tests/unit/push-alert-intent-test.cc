#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/push-delivery/push-alert-intent.hxx>

namespace
{
push_alert_intent::Input panicAlert()
{
  return {.intentId = "0a1b2c3d",
          .notificationId = 42,
          .type = "guard.panic",
          .title = "Alerta crítica",
          .body = "Alguien de casa ha pulsado el botón de pánico.",
          .urgency = "critical",
          .episode = "guard:panic:7",
          .ringFollows = true,
          .issuedAt = 1000,
          .expiresAt = 4600};
}
}

TEST_CASE("a critical alert of a ring carries kind alert, urgency, episode and ringFollows")
{
  const Json::Value intent = push_alert_intent::build(panicAlert());
  CHECK(intent["kind"].asString() == "alert");
  CHECK(intent["intentId"].asString() == "0a1b2c3d");
  CHECK(intent["urgency"].asString() == "critical");
  CHECK(intent["episode"].asString() == "guard:panic:7");
  CHECK(intent["ringFollows"].asBool());
  CHECK(intent["notificationId"].asInt64() == 42);
  CHECK(intent["type"].asString() == "guard.panic");
  CHECK(intent["title"].asString() == "Alerta crítica");
  CHECK(intent["deepLink"].asString() == "argus://notification?notificationId=42");
  CHECK(intent["iat"].asInt64() == 1000);
  CHECK(intent["exp"].asInt64() == 4600);
}

TEST_CASE("an alert whose ring is not planned carries the episode but no ringFollows")
{
  auto input = panicAlert();
  input.ringFollows = false;
  const Json::Value intent = push_alert_intent::build(input);
  CHECK(intent["episode"].asString() == "guard:panic:7");
  CHECK_FALSE(intent.isMember("ringFollows"));
  CHECK(intent["kind"].asString() == "alert");
}

TEST_CASE("an alert without an episode carries neither episode nor ringFollows")
{
  auto input = panicAlert();
  input.episode.clear();
  input.ringFollows = false;
  input.urgency = "normal";
  const Json::Value intent = push_alert_intent::build(input);
  CHECK_FALSE(intent.isMember("episode"));
  CHECK_FALSE(intent.isMember("ringFollows"));
  CHECK(intent["urgency"].asString() == "normal");
  CHECK(intent["kind"].asString() == "alert");
}
