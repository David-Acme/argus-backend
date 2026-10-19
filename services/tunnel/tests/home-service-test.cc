#include <link/client/home-service.hxx>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <set>
#include <string_view>

using namespace tunnel::link;

TEST_CASE("the closed set holds exactly the ten services the device names may carry")
{
  std::set<std::string_view> names;
  for (const HomeService service : kHomeServices)
    names.insert(homeServiceToString(service));
  const std::set<std::string_view> expected{
      "auth",         "identity", "sync", "camera", "productivity",
      "notification", "guard",    "settings", "rtc", "turn"};
  CHECK(kHomeServices.size() == 10);
  CHECK(names.size() == kHomeServices.size());
  CHECK(names == expected);
}

TEST_CASE("each service spells its name and parses back to itself")
{
  for (const HomeService service : kHomeServices) {
    const std::optional<HomeService> parsed =
        homeServiceFromString(homeServiceToString(service));
    CHECK(parsed.has_value());
    CHECK(parsed == service);
  }
}

TEST_CASE("the call services are named rtc and turn")
{
  CHECK(homeServiceToString(HomeService::Rtc) == "rtc");
  CHECK(homeServiceToString(HomeService::Turn) == "turn");
  CHECK(homeServiceFromString("rtc") == HomeService::Rtc);
  CHECK(homeServiceFromString("turn") == HomeService::Turn);
}

TEST_CASE("a name outside the closed set is refused")
{
  const std::array<std::string_view, 11> refused{
      "", "media", "tv", "webrtc", "rtc2", "turnx", "turn-", "RTC", "Turn",
      "camera2", "livekit"};
  for (const std::string_view name : refused) {
    INFO(name);
    CHECK_FALSE(homeServiceFromString(name).has_value());
  }
}
