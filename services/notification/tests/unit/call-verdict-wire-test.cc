#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/call/infra/nats-response-verdict-sink.hxx>
#include <text/json-util.hxx>

TEST_CASE("a verdict keeps one message id per response, verdict and user, and carries what guard reads")
{
  const ResponseVerdictEvent event{.responseId = 41,
                                   .kind = "guard_episode",
                                   .threadKey = "guard:episode:7",
                                   .episodeId = 7,
                                   .environmentId = 2,
                                   .verdict = "false_alarm",
                                   .userId = 5,
                                   .at = 1'700'000'000};
  CHECK(response_verdict_wire::messageId(event) == "response-verdict:41:false_alarm:5");
  CHECK(response_verdict_wire::messageId(event) == response_verdict_wire::messageId(event));
  ResponseVerdictEvent other = event;
  other.verdict = "real";
  CHECK(response_verdict_wire::messageId(other) != response_verdict_wire::messageId(event));
  other = event;
  other.userId = 6;
  CHECK(response_verdict_wire::messageId(other) != response_verdict_wire::messageId(event));
  const Json::Value payload = json_util::fromString(response_verdict_wire::payload(event));
  CHECK(payload["kind"].asString() == "guard_episode");
  CHECK(payload["episodeId"].asInt64() == 7);
  CHECK(payload["verdict"].asString() == "false_alarm");
  CHECK(payload["at"].asInt64() == 1'700'000'000);
  CHECK(payload["responseId"].asInt64() == 41);
}
