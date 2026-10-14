#include <shared/services/onvif/onvif-parse.hxx>

#include <shared/services/onvif/onvif-soap.hxx>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ostream>
#include <set>
#include <sstream>
#include <text/iso-time.hxx>
#include <vector>

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

std::string_view afterAuthority(std::string_view address)
{
  const auto scheme = address.find("://");
  if (scheme == std::string_view::npos)
    return address;
  address.remove_prefix(scheme + 3);
  const auto slash = address.find('/');
  return slash == std::string_view::npos ? std::string_view("/") : address.substr(slash);
}

std::string checkedTarget(std::string_view target)
{
  if (target.empty() || target.front() != '/' || target.size() > kMaxPathLength ||
      !std::ranges::all_of(target, plainPathCharacter))
    return {};
  return std::string(target);
}

std::string declarationFor(std::string_view qualified)
{
  const auto colon = qualified.find(':');
  return colon == std::string_view::npos ? std::string("xmlns") : "xmlns:" + std::string(qualified.substr(0, colon));
}

bool declares(std::string_view attribute)
{
  return attribute == "xmlns" || attribute.starts_with("xmlns:");
}

std::set<std::string> declarationsUsed(const pugi::xml_node& root)
{
  std::set<std::string> used;
  std::vector<pugi::xml_node> pending{root};
  while (!pending.empty()) {
    const pugi::xml_node node = pending.back();
    pending.pop_back();
    used.insert(declarationFor(node.name()));
    for (const auto attribute : node.attributes()) {
      const std::string_view name = attribute.name();
      if (!declares(name) && name.find(':') != std::string_view::npos)
        used.insert(declarationFor(name));
    }
    for (const auto child : node.children()) {
      if (child.type() == pugi::node_element)
        pending.push_back(child);
    }
  }
  return used;
}

void printSelfContained(const pugi::xml_node& node, std::ostream& out)
{
  pugi::xml_document fragment;
  auto copy = fragment.append_copy(node);
  for (const std::string& declaration : declarationsUsed(node)) {
    if (copy.attribute(declaration.c_str()))
      continue;
    for (pugi::xml_node scope = node.parent(); scope; scope = scope.parent()) {
      if (const auto inScope = scope.attribute(declaration.c_str())) {
        copy.append_attribute(declaration.c_str()) = inScope.value();
        break;
      }
    }
  }
  copy.print(out, "", pugi::format_raw);
}

constexpr std::array<std::string_view, 2> kMotionTopics{"RuleEngine/CellMotionDetector/Motion",
                                                       "VideoSource/MotionAlarm"};
constexpr std::array<std::string_view, 2> kMotionItems{"IsMotion", "State"};

bool motionTopic(std::string_view topic)
{
  return std::ranges::any_of(kMotionTopics, [topic](std::string_view wanted) { return topic.ends_with(wanted); });
}

bool namesTopic(const pugi::xml_node& set, std::string_view topic)
{
  pugi::xml_node node = set;
  while (node && !topic.empty()) {
    const auto slash = topic.find('/');
    node = onvif_soap::child(node, topic.substr(0, slash));
    topic = slash == std::string_view::npos ? std::string_view() : topic.substr(slash + 1);
  }
  return !node.empty();
}

std::optional<bool> motionValue(const pugi::xml_node& data)
{
  for (const auto item : data.children()) {
    if (item.type() != pugi::node_element || onvif_soap::localName(item) != "SimpleItem")
      continue;
    const std::string_view name = item.attribute("Name").value();
    if (std::ranges::find(kMotionItems, name) == kMotionItems.end())
      continue;
    const std::string_view value = item.attribute("Value").value();
    if (value == "true" || value == "1")
      return true;
    if (value == "false" || value == "0")
      return false;
  }
  return std::nullopt;
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
  out.events = pathAt(capabilities, "Events");
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
  const std::string_view rest = afterAuthority(xaddr);
  return checkedTarget(rest.substr(0, rest.find_first_of("?#")));
}

std::string onvif_parse::targetOf(std::string_view address)
{
  const std::string_view rest = afterAuthority(address);
  return checkedTarget(rest.substr(0, rest.find('#')));
}

std::optional<int64_t> onvif_parse::epochOf(std::string_view dateTime)
{
  std::string plain(dateTime);
  if (const auto dot = plain.find('.'); dot != std::string::npos && dot > plain.find('T')) {
    auto end = dot + 1;
    while (end < plain.size() && plain[end] >= '0' && plain[end] <= '9')
      ++end;
    plain.erase(dot, end - dot);
  }
  return iso_time::parse(plain);
}

int64_t onvif_parse::lifetimeOf(const pugi::xml_node& response)
{
  const auto current = epochOf(onvif_soap::textOf(onvif_soap::child(response, "CurrentTime")));
  const auto termination = epochOf(onvif_soap::textOf(onvif_soap::child(response, "TerminationTime")));
  if (!current || !termination || *termination <= *current)
    return 0;
  return *termination - *current;
}

std::optional<OnvifSubscription> onvif_parse::subscription(const pugi::xml_node& response)
{
  const pugi::xml_node reference = onvif_soap::child(response, "SubscriptionReference");
  const std::string address = onvif_soap::textOf(onvif_soap::child(reference, "Address"));
  std::string path = targetOf(address);
  if (path.empty())
    return std::nullopt;
  std::ostringstream references;
  for (const auto node : onvif_soap::child(reference, "ReferenceParameters").children()) {
    if (node.type() == pugi::node_element)
      printSelfContained(node, references);
  }
  return OnvifSubscription{.address = address,
                           .path = std::move(path),
                           .references = references.str(),
                           .lifetimeSeconds = lifetimeOf(response)};
}

std::optional<bool> onvif_parse::publishesMotion(const pugi::xml_node& response)
{
  const pugi::xml_node set = onvif_soap::child(response, "TopicSet");
  if (!set)
    return std::nullopt;
  return std::ranges::any_of(kMotionTopics, [&set](std::string_view topic) { return namesTopic(set, topic); });
}

OnvifPull onvif_parse::pulled(const pugi::xml_node& response)
{
  OnvifPull out{.motions = {}, .lifetimeSeconds = lifetimeOf(response)};
  for (const auto notification : response.children()) {
    if (notification.type() != pugi::node_element ||
        onvif_soap::localName(notification) != "NotificationMessage")
      continue;
    if (!motionTopic(onvif_soap::textOf(onvif_soap::child(notification, "Topic"))))
      continue;
    const pugi::xml_node message = onvif_soap::path(notification, {"Message", "Message"});
    const std::string_view operation = message.attribute("PropertyOperation").value();
    if (operation == "Deleted")
      continue;
    const auto moving = motionValue(onvif_soap::child(message, "Data"));
    if (!moving)
      continue;
    out.motions.push_back({.moving = *moving});
  }
  return out;
}
