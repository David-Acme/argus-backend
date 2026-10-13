#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/remote/services/remote-link-feed.hxx>
#include <feature/remote/services/remote-link-gate.hxx>
#include <routes/remote-link.hxx>
#include <shared/services/context/user-context.hxx>

#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kHomeId = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kDomain = "argus-tunnel.lynk.page";
constexpr int64_t kT0 = 1'790'000'000'000;
constexpr int64_t kSince = 1'790'000'000;

struct FrameFields
{
  std::string_view link;
  int64_t since;
  std::string_view homeId;
  std::string_view domain;
};

std::string frameOf(const FrameFields& fields)
{
  Json::Value json(Json::objectValue);
  json["homeId"] = std::string(fields.homeId);
  json["domain"] = std::string(fields.domain);
  json["link"] = std::string(fields.link);
  json["since"] = static_cast<Json::Int64>(fields.since);
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, json);
}

std::string frame(std::string_view link, int64_t since = kSince)
{
  return frameOf({.link = link, .since = since, .homeId = kHomeId, .domain = kDomain});
}

std::optional<routes::RemoteLinkState> stateOf(routes::RemoteLink link, int64_t since)
{
  return routes::RemoteLinkState{
      .homeId = std::string(kHomeId), .domain = std::string(kDomain), .link = link, .since = since};
}

std::string withField(const std::string& key, const Json::Value& value)
{
  Json::Value json(Json::objectValue);
  json["homeId"] = std::string(kHomeId);
  json["domain"] = std::string(kDomain);
  json["link"] = "up";
  json["since"] = static_cast<Json::Int64>(kSince);
  json[key] = value;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, json);
}

RemoteLinkGate gate()
{
  return RemoteLinkGate({.staleAfterMs = 90'000, .throttleMs = 5'000});
}

RemoteLinkFeed feed()
{
  return RemoteLinkFeed({.bus = nullptr},
                        {.subject = "argus.tunnel.v1.link",
                         .gate = {.staleAfterMs = 90'000, .throttleMs = 5'000},
                         .tickSeconds = 1.0,
                         .refusalLogWindowMs = 60'000});
}
}

TEST_CASE("a valid frame becomes the state, with the four fields as sent")
{
  auto subject = gate();
  CHECK_FALSE(subject.state().has_value());
  CHECK(subject.ingest(frame("up"), kT0) == routes::RemoteLinkRefusal::None);
  CHECK(subject.state() == stateOf(routes::RemoteLink::Up, kSince));
}

TEST_CASE("a frame whose home id is not 32 hex characters is dropped")
{
  auto subject = gate();
  CHECK(subject.ingest(withField("homeId", "not-a-home"), kT0) == routes::RemoteLinkRefusal::HomeId);
  CHECK(subject.ingest(withField("homeId", "0123456789ABCDEF0123456789ABCDEF"), kT0) ==
        routes::RemoteLinkRefusal::HomeId);
  CHECK_FALSE(subject.state().has_value());
}

TEST_CASE("a frame whose domain is not a DNS name is dropped")
{
  auto subject = gate();
  CHECK(subject.ingest(withField("domain", "lynk"), kT0) == routes::RemoteLinkRefusal::Domain);
  CHECK(subject.ingest(withField("domain", "argus..page"), kT0) == routes::RemoteLinkRefusal::Domain);
  CHECK_FALSE(subject.state().has_value());
}

TEST_CASE("a frame whose link is not one of the closed enum's values is dropped")
{
  auto subject = gate();
  CHECK(subject.ingest(withField("link", "online"), kT0) == routes::RemoteLinkRefusal::Link);
  CHECK(subject.ingest(withField("link", 1), kT0) == routes::RemoteLinkRefusal::Link);
  CHECK_FALSE(subject.state().has_value());
}

TEST_CASE("a frame whose since is not a positive integer is dropped")
{
  auto subject = gate();
  CHECK(subject.ingest(withField("since", 0), kT0) == routes::RemoteLinkRefusal::Since);
  CHECK(subject.ingest(withField("since", -1), kT0) == routes::RemoteLinkRefusal::Since);
  CHECK(subject.ingest(withField("since", 1.5), kT0) == routes::RemoteLinkRefusal::Since);
  CHECK_FALSE(subject.state().has_value());
}

TEST_CASE("a failing frame leaves the last valid state alone")
{
  auto subject = gate();
  REQUIRE(subject.ingest(frame("up"), kT0) == routes::RemoteLinkRefusal::None);
  const auto valid = subject.state();
  CHECK(subject.ingest(withField("homeId", "x"), kT0 + 1) != routes::RemoteLinkRefusal::None);
  CHECK(subject.ingest(withField("domain", "x"), kT0 + 2) != routes::RemoteLinkRefusal::None);
  CHECK(subject.ingest(withField("link", "x"), kT0 + 3) != routes::RemoteLinkRefusal::None);
  CHECK(subject.ingest(withField("since", 0), kT0 + 4) != routes::RemoteLinkRefusal::None);
  CHECK(subject.ingest("not json", kT0 + 5) == routes::RemoteLinkRefusal::NotAnObject);
  CHECK(subject.state() == valid);
}

TEST_CASE("one frame goes out at once, and a change inside 5 seconds waits for the window")
{
  auto subject = gate();
  subject.ingest(frame("up"), kT0);
  REQUIRE(subject.shouldSend(kT0));
  subject.markSent(kT0);
  CHECK_FALSE(subject.shouldSend(kT0));
  subject.ingest(frame("down", 1'790'000'001), kT0 + 1'000);
  CHECK_FALSE(subject.shouldSend(kT0 + 1'000));
  CHECK_FALSE(subject.shouldSend(kT0 + 4'999));
  CHECK(subject.shouldSend(kT0 + 5'000));
}

TEST_CASE("the trailing frame carries the latest state of the window")
{
  auto subject = gate();
  subject.ingest(frame("up"), kT0);
  subject.markSent(kT0);
  subject.ingest(frame("down", 1'790'000'001), kT0 + 1'000);
  subject.ingest(frame("connecting", 1'790'000'002), kT0 + 2'000);
  REQUIRE(subject.shouldSend(kT0 + 5'000));
  CHECK(subject.state() == stateOf(routes::RemoteLink::Connecting, 1'790'000'002));
  subject.markSent(kT0 + 5'000);
  CHECK_FALSE(subject.shouldSend(kT0 + 20'000));
}

TEST_CASE("nothing is sent when the state is back where the last frame left it")
{
  auto subject = gate();
  subject.ingest(frame("up"), kT0);
  subject.markSent(kT0);
  subject.ingest(frame("down", 1'790'000'001), kT0 + 1'000);
  subject.ingest(frame("up", 1'790'000'002), kT0 + 2'000);
  CHECK_FALSE(subject.shouldSend(kT0 + 5'000));
  CHECK_FALSE(subject.shouldSend(kT0 + 60'000));
}

TEST_CASE("a new home id or domain is a change, a since alone is not")
{
  auto subject = gate();
  subject.ingest(frame("up"), kT0);
  subject.markSent(kT0);
  subject.ingest(frame("up", 1'790'000'500), kT0 + 30'000);
  CHECK_FALSE(subject.shouldSend(kT0 + 30'000));
  subject.ingest(frameOf({.link = "up",
                          .since = 1'790'000'500,
                          .homeId = "fedcba9876543210fedcba9876543210",
                          .domain = kDomain}),
                 kT0 + 31'000);
  CHECK(subject.shouldSend(kT0 + 31'000));
  subject.markSent(kT0 + 31'000);
  subject.ingest(frameOf({.link = "up",
                          .since = 1'790'000'500,
                          .homeId = "fedcba9876543210fedcba9876543210",
                          .domain = "relay.example.com"}),
                 kT0 + 40'000);
  CHECK(subject.shouldSend(kT0 + 40'000));
}

TEST_CASE("after 90 seconds with no frame the link reads down and the home id stays")
{
  auto subject = gate();
  subject.ingest(frame("up"), kT0);
  subject.markSent(kT0);
  CHECK_FALSE(subject.expire(kT0 + 90'000));
  CHECK(subject.state() == stateOf(routes::RemoteLink::Up, kSince));
  subject.ingest(withField("link", "nonsense"), kT0 + 60'000);
  CHECK(subject.expire(kT0 + 90'001));
  CHECK(subject.state() == stateOf(routes::RemoteLink::Down, kT0 / 1000));
  CHECK(subject.shouldSend(kT0 + 90'001));
  CHECK_FALSE(subject.expire(kT0 + 200'000));
  subject.markSent(kT0 + 90'001);
  subject.ingest(frame("up"), kT0 + 100'000);
  CHECK(subject.shouldSend(kT0 + 100'000));
}

TEST_CASE("no frame, no state, nothing to send and nothing to expire")
{
  auto subject = gate();
  CHECK_FALSE(subject.shouldSend(kT0));
  CHECK_FALSE(subject.expire(kT0 + 1'000'000));
}

TEST_CASE("refusals are logged once per minute, with the count of every frame dropped")
{
  RefusalLog log(60'000);
  CHECK(log.record(kT0) == 1U);
  CHECK_FALSE(log.record(kT0 + 1'000).has_value());
  CHECK_FALSE(log.record(kT0 + 59'999).has_value());
  CHECK(log.record(kT0 + 60'000) == 3U);
  CHECK_FALSE(log.record(kT0 + 61'000).has_value());
  CHECK(log.record(kT0 + 200'000) == 2U);
}

TEST_CASE("the feed puts each valid state into the context and keeps it through a bad frame")
{
  userContext().setRemote(std::nullopt);
  auto subject = feed();
  subject.ingest(frame("up"), kT0);
  CHECK(userContext().remote() == stateOf(routes::RemoteLink::Up, kSince));
  subject.ingest(withField("link", "sideways"), kT0 + 1'000);
  CHECK(userContext().remote() == stateOf(routes::RemoteLink::Up, kSince));
  subject.tick(kT0 + 90'001);
  CHECK(userContext().remote() == stateOf(routes::RemoteLink::Down, kT0 / 1000));
  userContext().setRemote(std::nullopt);
}
