#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <http/logical-routes.hxx>
#include <http/route-announcements.hxx>
#include <routes/service-discovery.hxx>
#include <string>
#include <vector>

TEST_CASE("the leading segment of every registered pattern is the route set")
{
  const std::vector<std::string> routes = logicalRoutesFrom(
      {"/camera/list", "/camera/{1}", "/zone", "/sync", "/media"});

  REQUIRE(routes.size() == 4);
  CHECK(routes[0] == "camera");
  CHECK(routes[1] == "media");
  CHECK(routes[2] == "sync");
  CHECK(routes[3] == "zone");
}

TEST_CASE("health, the root and patterns without a segment are not routes")
{
  const std::vector<std::string> routes =
      logicalRoutesFrom({"/health", "/", "", "camera", "//"});

  CHECK(routes.empty());
}

TEST_CASE("a route is announced once, at the listener's port")
{
  const std::vector<MdnsInstance> instances =
      routeAnnouncementsFor({"camera", "guard"}, {.port = 7026, .tls = true});

  REQUIRE(instances.size() == 2);
  for (const MdnsInstance& instance : instances) {
    CHECK(instance.serviceType == std::string(routes::kServiceType));
    CHECK(instance.port == 7026);
    REQUIRE(instance.txt.size() == 2);
    CHECK(instance.txt[0].first == std::string(routes::kTxtPath));
    CHECK(instance.txt[0].second == instance.path);
    CHECK(instance.txt[1].first == std::string(routes::kTxtHttps));
    CHECK(instance.txt[1].second == "true");
  }
  CHECK(instances[0].path == "camera");
  CHECK(instances[1].path == "guard");
}

TEST_CASE("a plain listener announces its routes without https")
{
  const std::vector<MdnsInstance> instances =
      routeAnnouncementsFor({"auth"}, {.port = 7042, .tls = false});

  REQUIRE(instances.size() == 1);
  REQUIRE(instances[0].txt.size() == 1);
  CHECK(instances[0].txt[0].first == std::string(routes::kTxtPath));
  CHECK(instances[0].txt[0].second == "auth");
  CHECK(instances[0].port == 7042);
}
