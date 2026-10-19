#include <doctest/doctest.h>

#include <runtime/host-time-zone.hxx>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
struct TzEnvironment
{
  TzEnvironment() { unsetenv("TZ"); }
  ~TzEnvironment() { unsetenv("TZ"); }
};
}

TEST_CASE("the zone name is the path after the zoneinfo directory")
{
  CHECK(zoneNameFromPath("/usr/share/zoneinfo/America/Lima") == "America/Lima");
  CHECK(zoneNameFromPath("../usr/share/zoneinfo/Europe/Madrid") == "Europe/Madrid");
  CHECK(zoneNameFromPath("/usr/share/zoneinfo/posix/Asia/Tokyo") == "Asia/Tokyo");
  CHECK(zoneNameFromPath("/etc/localtime").empty());
}

TEST_CASE("TZ names the zone when it is set")
{
  TzEnvironment environment;
  setenv("TZ", "America/Lima", 1);
  CHECK(hostTimeZone() == "America/Lima");
  setenv("TZ", ":Europe/Madrid", 1);
  CHECK(hostTimeZone() == "Europe/Madrid");
  setenv("TZ", "/usr/share/zoneinfo/Asia/Tokyo", 1);
  CHECK(hostTimeZone() == "Asia/Tokyo");
}

TEST_CASE("without TZ the localtime symlink names the zone")
{
  TzEnvironment environment;
  const auto directory = std::filesystem::temp_directory_path() / "argus-host-time-zone-test";
  std::filesystem::create_directories(directory);
  const auto link = directory / "localtime";
  std::filesystem::remove(link);
  std::filesystem::create_symlink("/usr/share/zoneinfo/America/Lima", link);
  CHECK(hostTimeZone(link.string()) == "America/Lima");
  std::filesystem::remove_all(directory);
}
