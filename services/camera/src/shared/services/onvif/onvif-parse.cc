#include <shared/services/onvif/onvif-parse.hxx>

#include <shared/services/onvif/onvif-soap.hxx>

#include <algorithm>
#include <charconv>
#include <chrono>

namespace
{
constexpr size_t kMaxPathLength = 200;

std::optional<int> numberOf(const pugi::xml_node& node)
{
  const std::string text = onvif_soap::textOf(node);
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc() || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::string pathAt(const pugi::xml_node& capabilities, std::string_view service)
{
  return onvif_parse::pathOf(onvif_soap::textOf(onvif_soap::path(capabilities, {service, "XAddr"})));
}

bool plainPathCharacter(char c)
{
  return c > ' ' && c < 0x7F;
}
}

std::optional<int64_t> onvif_parse::deviceEpoch(const pugi::xml_node& response)
{
  const pugi::xml_node utc = onvif_soap::path(response, {"SystemDateAndTime", "UTCDateTime"});
  if (!utc)
    return std::nullopt;
  const auto year = numberOf(onvif_soap::path(utc, {"Date", "Year"}));
  const auto month = numberOf(onvif_soap::path(utc, {"Date", "Month"}));
  const auto day = numberOf(onvif_soap::path(utc, {"Date", "Day"}));
  const auto hour = numberOf(onvif_soap::path(utc, {"Time", "Hour"}));
  const auto minute = numberOf(onvif_soap::path(utc, {"Time", "Minute"}));
  const auto second = numberOf(onvif_soap::path(utc, {"Time", "Second"}));
  if (!year || !month || !day || !hour || !minute || !second)
    return std::nullopt;
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31)
    return std::nullopt;
  const std::chrono::year_month_day date{std::chrono::year{*year},
                                         std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok() || *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59 || *second < 0 ||
      *second > 59)
    return std::nullopt;
  const auto at = std::chrono::sys_days{date} + std::chrono::hours{*hour} +
                  std::chrono::minutes{*minute} + std::chrono::seconds{*second};
  return std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
}

OnvifServices onvif_parse::services(const pugi::xml_node& response)
{
  const pugi::xml_node capabilities = onvif_soap::child(response, "Capabilities");
  OnvifServices out;
  if (std::string device = pathAt(capabilities, "Device"); !device.empty())
    out.device = std::move(device);
  out.media = pathAt(capabilities, "Media");
  out.ptz = pathAt(capabilities, "PTZ");
  out.imaging = pathAt(capabilities, "Imaging");
  return out;
}

OnvifDeviceInfo onvif_parse::deviceInfo(const pugi::xml_node& response)
{
  return {.manufacturer = onvif_soap::textOf(onvif_soap::child(response, "Manufacturer")),
          .model = onvif_soap::textOf(onvif_soap::child(response, "Model")),
          .firmware = onvif_soap::textOf(onvif_soap::child(response, "FirmwareVersion")),
          .serial = onvif_soap::textOf(onvif_soap::child(response, "SerialNumber")),
          .hardwareId = onvif_soap::textOf(onvif_soap::child(response, "HardwareId"))};
}

std::vector<OnvifProfile> onvif_parse::profiles(const pugi::xml_node& response)
{
  std::vector<OnvifProfile> out;
  for (const auto node : response.children()) {
    if (node.type() != pugi::node_element || onvif_soap::localName(node) != "Profiles")
      continue;
    const std::string token = node.attribute("token").value();
    if (token.empty())
      continue;
    const pugi::xml_node resolution =
        onvif_soap::path(node, {"VideoEncoderConfiguration", "Resolution"});
    out.push_back({.token = token,
                   .name = onvif_soap::textOf(onvif_soap::child(node, "Name")),
                   .ptzConfiguration = onvif_soap::child(node, "PTZConfiguration").attribute("token").value(),
                   .width = numberOf(onvif_soap::child(resolution, "Width")).value_or(0),
                   .height = numberOf(onvif_soap::child(resolution, "Height")).value_or(0)});
  }
  return out;
}

std::vector<OnvifPreset> onvif_parse::presets(const pugi::xml_node& response)
{
  std::vector<OnvifPreset> out;
  for (const auto node : response.children()) {
    if (node.type() != pugi::node_element || onvif_soap::localName(node) != "Preset")
      continue;
    std::string token = node.attribute("token").value();
    if (token.empty())
      continue;
    out.push_back({.token = std::move(token), .name = onvif_soap::textOf(onvif_soap::child(node, "Name"))});
  }
  return out;
}

std::string onvif_parse::presetToken(const pugi::xml_node& response)
{
  return onvif_soap::textOf(onvif_soap::child(response, "PresetToken"));
}

std::string onvif_parse::pathOf(std::string_view xaddr)
{
  std::string_view rest = xaddr;
  if (const auto scheme = rest.find("://"); scheme != std::string_view::npos) {
    rest.remove_prefix(scheme + 3);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos)
      return "/";
    rest.remove_prefix(slash);
  }
  if (const auto query = rest.find_first_of("?#"); query != std::string_view::npos)
    rest = rest.substr(0, query);
  if (rest.empty() || rest.front() != '/' || rest.size() > kMaxPathLength ||
      !std::ranges::all_of(rest, plainPathCharacter))
    return {};
  return std::string(rest);
}
