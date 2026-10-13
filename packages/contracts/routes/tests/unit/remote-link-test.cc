#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <routes/remote-link.hxx>

#include <json/value.h>

#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kHomeId = "0123456789abcdef0123456789abcdef";

Json::Value validFrame()
{
  Json::Value json(Json::objectValue);
  json["homeId"] = std::string(kHomeId);
  json["domain"] = "argus-tunnel.lynk.page";
  json["link"] = "up";
  json["since"] = static_cast<Json::Int64>(1'790'000'000);
  return json;
}

routes::RemoteLinkRefusal refusalOf(const Json::Value& json)
{
  return routes::readRemoteLink(json).refusal;
}
}

TEST_CASE("the link vocabulary is the closed enum, with its own string pair")
{
  for (const auto link : {routes::RemoteLink::Up, routes::RemoteLink::Connecting, routes::RemoteLink::Down})
    CHECK(routes::remoteLinkFromString(routes::remoteLinkToString(link)) == link);
  CHECK(routes::remoteLinkToString(routes::RemoteLink::Connecting) == "connecting");
  CHECK_FALSE(routes::remoteLinkFromString("UP").has_value());
  CHECK_FALSE(routes::remoteLinkFromString("flapping").has_value());
  CHECK_FALSE(routes::remoteLinkFromString("").has_value());
}

TEST_CASE("a home id is 32 lowercase hexadecimal characters")
{
  CHECK(routes::isHomeId(kHomeId));
  CHECK_FALSE(routes::isHomeId("0123456789ABCDEF0123456789ABCDEF"));
  CHECK_FALSE(routes::isHomeId(kHomeId.substr(1)));
  CHECK_FALSE(routes::isHomeId(std::string(kHomeId) + "0"));
  CHECK_FALSE(routes::isHomeId("0123456789abcdef0123456789abcdeg"));
}

TEST_CASE("a domain is a lowercase DNS name of two labels or more")
{
  CHECK(routes::isDomainName("argus-tunnel.lynk.page"));
  CHECK(routes::isDomainName("lynk.page"));
  CHECK(routes::isDomainName("a1.example.com"));
  CHECK_FALSE(routes::isDomainName("localhost"));
  CHECK_FALSE(routes::isDomainName("argus-tunnel.lynk.page."));
  CHECK_FALSE(routes::isDomainName(".lynk.page"));
  CHECK_FALSE(routes::isDomainName("argus..page"));
  CHECK_FALSE(routes::isDomainName("Argus-Tunnel.lynk.page"));
  CHECK_FALSE(routes::isDomainName("-argus.lynk.page"));
  CHECK_FALSE(routes::isDomainName("argus-.lynk.page"));
  CHECK_FALSE(routes::isDomainName("argus.lynk.p4ge"));
  CHECK_FALSE(routes::isDomainName("argus.lynk.p"));
  CHECK_FALSE(routes::isDomainName("argus_tunnel.lynk.page"));
  CHECK_FALSE(routes::isDomainName(std::string(64, 'a') + ".page"));
  std::string longName;
  while (longName.size() <= 253)
    longName += "abcdefghij.";
  CHECK_FALSE(routes::isDomainName(longName + "page"));
}

TEST_CASE("a valid frame reads back as the state it was written from")
{
  const routes::RemoteLinkState state{.homeId = std::string(kHomeId),
                                      .domain = "argus-tunnel.lynk.page",
                                      .link = routes::RemoteLink::Connecting,
                                      .since = 1'790'000'123};
  const auto json = routes::remoteLinkToJson(state);
  CHECK(json["link"].asString() == "connecting");
  CHECK(json.getMemberNames().size() == 4);
  const auto reading = routes::readRemoteLink(json);
  CHECK(reading.refusal == routes::RemoteLinkRefusal::None);
  REQUIRE(reading.state.has_value());
  CHECK(reading.state == state);
}

TEST_CASE("a frame is refused when its home id is not 32 lowercase hex characters")
{
  auto frame = validFrame();
  frame["homeId"] = "0123456789ABCDEF0123456789ABCDEF";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::HomeId);
  frame["homeId"] = 42;
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::HomeId);
  frame.removeMember("homeId");
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::HomeId);
}

TEST_CASE("a frame is refused when its domain is not a DNS name")
{
  auto frame = validFrame();
  frame["domain"] = "lynk";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Domain);
  frame["domain"] = "argus tunnel.lynk.page";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Domain);
  frame["domain"] = Json::Value(Json::nullValue);
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Domain);
}

TEST_CASE("a frame is refused when its link is not one of the closed enum's values")
{
  auto frame = validFrame();
  frame["link"] = "UP";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Link);
  frame["link"] = "flapping";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Link);
  frame["link"] = 1;
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Link);
}

TEST_CASE("a frame is refused when its since is not a positive integer")
{
  auto frame = validFrame();
  frame["since"] = 0;
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Since);
  frame["since"] = -5;
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Since);
  frame["since"] = "1790000000";
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Since);
  frame["since"] = 1790000000.0;
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Since);
  frame.removeMember("since");
  CHECK(refusalOf(frame) == routes::RemoteLinkRefusal::Since);
}

TEST_CASE("anything but an object is refused, and every refusal names its check")
{
  CHECK(refusalOf(Json::Value("up")) == routes::RemoteLinkRefusal::NotAnObject);
  CHECK(refusalOf(Json::Value(Json::arrayValue)) == routes::RemoteLinkRefusal::NotAnObject);
  CHECK_FALSE(routes::readRemoteLink(Json::Value(Json::nullValue)).state.has_value());
  for (const auto refusal : {routes::RemoteLinkRefusal::NotAnObject, routes::RemoteLinkRefusal::HomeId,
                             routes::RemoteLinkRefusal::Domain, routes::RemoteLinkRefusal::Link,
                             routes::RemoteLinkRefusal::Since})
    CHECK_FALSE(routes::remoteLinkRefusalToString(refusal).empty());
}
