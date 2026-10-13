#include "host-time-zone.hxx"

#include <array>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace
{
constexpr std::string_view kZoneinfoMarker = "/zoneinfo/";
constexpr std::string_view kPosixPrefix = "posix/";
constexpr std::string_view kUtc = "UTC";
constexpr std::string_view kTimezoneFile = "/etc/timezone";
constexpr std::size_t kLinkCapacity = 4096;

std::string fromEnvironment()
{
  const char* value = std::getenv("TZ");
  if (value == nullptr)
    return {};
  std::string zone = value;
  if (zone.starts_with(':'))
    zone.erase(0, 1);
  if (zone.starts_with('/'))
    return zoneNameFromPath(zone);
  return zone;
}

std::string fromLocaltimeLink(std::string_view localtimePath)
{
  const std::string path(localtimePath);
  std::array<char, kLinkCapacity> target{};
  const ssize_t written = readlink(path.c_str(), target.data(), target.size());
  if (written <= 0 || static_cast<std::size_t>(written) >= kLinkCapacity)
    return {};
  return zoneNameFromPath(std::string_view(target.data(), static_cast<std::size_t>(written)));
}

std::string fromTimezoneFile()
{
  std::ifstream file{std::string(kTimezoneFile)};
  std::string line;
  if (!std::getline(file, line))
    return {};
  while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
    line.pop_back();
  return line;
}
}

std::string zoneNameFromPath(std::string_view path)
{
  const std::size_t marker = path.rfind(kZoneinfoMarker);
  if (marker == std::string_view::npos)
    return {};
  std::string zone(path.substr(marker + kZoneinfoMarker.size()));
  if (zone.starts_with(kPosixPrefix))
    zone.erase(0, kPosixPrefix.size());
  return zone;
}

std::string hostTimeZone(std::string_view localtimePath)
{
  if (std::string zone = fromEnvironment(); !zone.empty())
    return zone;
  if (std::string zone = fromLocaltimeLink(localtimePath); !zone.empty())
    return zone;
  if (std::string zone = fromTimezoneFile(); !zone.empty())
    return zone;
  return std::string(kUtc);
}
