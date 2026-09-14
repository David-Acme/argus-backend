#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <guard-assessment.hxx>

TEST_CASE("a vision tool call asks for a specific question")
{
  const auto decision = guard_assessment::parseDecision(
      R"({"tool":"vision.describe","prompt":"is the person carrying a bag?"})");
  CHECK(decision.kind == GuardDecisionKind::ToolCall);
  CHECK(decision.tool.tool == "vision.describe");
  CHECK(decision.tool.prompt == "is the person carrying a bag?");
}

TEST_CASE("a vision tool call without a prompt falls back to the default")
{
  const auto decision =
      guard_assessment::parseDecision(R"({"tool":"vision.describe"})");
  CHECK(decision.kind == GuardDecisionKind::ToolCall);
  CHECK_FALSE(decision.tool.prompt.empty());
}

TEST_CASE("listening carries its duration and physical tools are rejected")
{
  const auto listen =
      guard_assessment::parseDecision(R"({"tool":"camera.listen","seconds":5})");
  CHECK(listen.kind == GuardDecisionKind::ToolCall);
  CHECK(listen.tool.tool == "camera.listen");
  CHECK(listen.tool.seconds == 5);

  CHECK(guard_assessment::parseDecision(
            R"({"tool":"camera.announce","text":"Leave"})")
            .kind == GuardDecisionKind::Invalid);
  CHECK(guard_assessment::parseDecision(R"({"tool":"camera.alarm","seconds":8})")
            .kind == GuardDecisionKind::Invalid);
  CHECK(guard_assessment::parseDecision(R"({"tool":"camera.siren","enabled":true})")
            .kind == GuardDecisionKind::Invalid);
}

TEST_CASE("a final decision carries threat, tags and the spoken proposal")
{
  const auto decision = guard_assessment::parseDecision(
      R"({"tool":"final","threat":"high","veto":false,)"
      R"("tags":["masked","backpack"],"summary":"Masked person at the gate",)"
      R"("announce_text":"Leave the area","alarm":true,"siren":true,)"
      R"("reason":"masked"})");
  CHECK(decision.kind == GuardDecisionKind::Final);
  CHECK(decision.final.threat == "high");
  CHECK_FALSE(decision.final.veto);
  CHECK(decision.final.tags.size() == 2);
  CHECK(decision.final.summary == "Masked person at the gate");
  CHECK(decision.final.announceText == "Leave the area");
  CHECK(decision.final.reason == "masked");
}

TEST_CASE("a legacy decision without a tool still closes the case")
{
  const auto decision = guard_assessment::parseDecision(
      R"({"threat":"medium","veto":true,"tags":[],"summary":"visitor"})");
  CHECK(decision.kind == GuardDecisionKind::Final);
  CHECK(decision.final.threat == "medium");
  CHECK(decision.final.veto);
}

TEST_CASE("prose, malformed JSON and unknown tools are invalid turns")
{
  CHECK(guard_assessment::parseDecision("I think this is fine").kind ==
        GuardDecisionKind::Invalid);
  CHECK(guard_assessment::parseDecision(R"({"tool":42})").kind ==
        GuardDecisionKind::Invalid);
  CHECK(guard_assessment::parseDecision("{\"tool\":\"unknown\"}").kind ==
        GuardDecisionKind::Invalid);
}
