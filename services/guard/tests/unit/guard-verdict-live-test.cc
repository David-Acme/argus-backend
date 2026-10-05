#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <doctest/doctest.h>
#include <feature/guard/services/response-verdict-feed.hxx>
#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

using guard_test::GuardBoot;
using guard_test::scalar;

TEST_CASE("a verdict published into the stream while guard is away is reviewed once it attaches" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();
  static GuardBoot boot("guard-verdict-live-test");
  scalar("INSERT INTO guard_encounter (first_seen, last_seen) VALUES (1, 2)");
  const std::string episode = scalar("SELECT MAX(id) FROM guard_encounter");

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));
  const std::string suffix = std::to_string(::getpid());
  const ResponseVerdictFeedConfig config{.stream = "ARGUS_GUARD_VERDICT_TEST",
                                         .subject = "argus.test.guard.verdict",
                                         .durable = "verdict-live-" + suffix,
                                         .maxDeliver = 5};
  REQUIRE(bus.ensureStream({.name = config.stream,
                            .subjects = {config.subject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 60LL * 1000000000}));
  Json::Value event(Json::objectValue);
  event["kind"] = "guard_episode";
  event["episodeId"] = static_cast<Json::Int64>(std::stoll(episode));
  event["verdict"] = "false_alarm";
  event["at"] = static_cast<Json::Int64>(1'700'000'200);
  const std::string payload = json_util::toString(event);
  for (int copy = 0; copy < 2; ++copy)
    REQUIRE(bus.publishWithMsgId(
        {.subject = config.subject, .payload = payload, .msgId = "verdict:" + suffix + ":false_alarm"}));
  const auto stored = bus.streamInfo(config.stream);
  REQUIRE(stored.has_value());

  ResponseVerdictFeed feed(&bus, config);
  feed.start();
  const std::string label = "SELECT review_label FROM guard_encounter WHERE id = " + episode;
  for (int attempt = 0; attempt < 1000 && scalar(label) != "false_alarm"; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(scalar(label) == "false_alarm");
  for (int attempt = 0; attempt < 500 && !feed.drained(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(feed.drained());
  feed.requestStop();
  bus.drain();
}
