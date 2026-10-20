#include "proximity-readings.hxx"

#include <charconv>
#include <ranges>

namespace
{
std::string_view trimmed(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

struct EntityRef
{
  std::string_view entity;
  std::string_view domain;
};

std::string entityName(const EntityRef& ref)
{
  std::string_view name = trimmed(ref.entity);
  const std::string prefix = std::string(ref.domain) + ".";
  if (name.starts_with(prefix))
    name.remove_prefix(prefix.size());
  return std::string(name);
}

std::optional<double> numberOf(std::string_view text)
{
  const std::string_view trimmedText = trimmed(text);
  double value = 0.0;
  const auto [end, error] = std::from_chars(trimmedText.data(), trimmedText.data() + trimmedText.size(), value);
  if (trimmedText.empty() || error != std::errc{} || end != trimmedText.data() + trimmedText.size())
    return std::nullopt;
  return value;
}

std::string baseName(std::string_view entity)
{
  std::string_view name = trimmed(entity);
  if (const size_t dot = name.find('.'); dot != std::string_view::npos)
    name.remove_prefix(dot + 1);
  return std::string(name);
}

std::optional<bool> wordState(std::string_view word)
{
  if (word == "on" || word == "ON" || word == "On" || word == "true" || word == "detected")
    return true;
  if (word == "off" || word == "OFF" || word == "Off" || word == "false" || word == "clear")
    return false;
  return std::nullopt;
}
}

std::vector<BleHeard> bleHeardOf(const Json::Value& body)
{
  std::vector<BleHeard> heard;
  if (!body.isObject())
    return heard;
  const Json::Value& value = body.isMember("value") ? body["value"] : body["state"];
  if (!value.isString())
    return heard;
  const std::string text = value.asString();
  for (const auto part : std::views::split(std::string_view(text), ';')) {
    const std::string_view entry = trimmed(std::string_view(part.begin(), part.end()));
    const size_t comma = entry.find(',');
    if (entry.empty() || comma == std::string_view::npos)
      continue;
    const std::string_view uuid = trimmed(entry.substr(0, comma));
    const std::string_view rssiText = trimmed(entry.substr(comma + 1));
    int rssi = 0;
    const auto [end, error] = std::from_chars(rssiText.data(), rssiText.data() + rssiText.size(), rssi);
    if (uuid.empty() || error != std::errc{} || end != rssiText.data() + rssiText.size())
      continue;
    heard.push_back({.uuid = std::string(uuid), .rssi = rssi});
  }
  return heard;
}

std::optional<bool> radarPresenceOf(const Json::Value& body)
{
  if (!body.isObject())
    return std::nullopt;
  if (body["value"].isBool())
    return body["value"].asBool();
  if (body["state"].isString())
    return wordState(body["state"].asString());
  if (body["state"].isBool())
    return body["state"].asBool();
  return std::nullopt;
}

ProximityRequestPath bleRequestPath(std::string_view entity)
{
  return {.path = "/text_sensor/" + entityName({.entity = entity, .domain = "text_sensor"}), .bearer = false};
}

ProximityRequestPath radarRequestPath(const ProximitySensorRow& sensor)
{
  if (sensor.source == ProximitySensorSource::HomeAssistant)
    return {.path = "/api/states/" + std::string(trimmed(sensor.entityId)), .bearer = true};
  return {.path = "/binary_sensor/" + entityName({.entity = sensor.entityId, .domain = "binary_sensor"}), .bearer = false};
}

std::optional<std::vector<RadarPoint>> radarTargetsOf(const Json::Value& body)
{
  if (!body.isObject())
    return std::nullopt;
  const Json::Value& value = body.isMember("value") ? body["value"] : body["state"];
  if (!value.isString())
    return std::nullopt;
  const std::string text = value.asString();
  std::vector<RadarPoint> points;
  for (const auto part : std::views::split(std::string_view(text), ';')) {
    const std::string_view entry = trimmed(std::string_view(part.begin(), part.end()));
    const size_t comma = entry.find(',');
    if (entry.empty() || comma == std::string_view::npos)
      continue;
    const std::optional<double> x = numberOf(entry.substr(0, comma));
    const std::optional<double> y = numberOf(entry.substr(comma + 1));
    if (!x || !y || (*x == 0.0 && *y == 0.0))
      continue;
    points.push_back({.xMm = *x, .yMm = *y});
  }
  return points;
}

ProximityRequestPath radarTargetsPath(const RadarTargetsPathInput& input)
{
  const std::string entity = baseName(input.sensor.entityId) + std::string(input.suffix);
  if (input.sensor.source == ProximitySensorSource::HomeAssistant)
    return {.path = "/api/states/sensor." + entity, .bearer = true};
  return {.path = "/text_sensor/" + entity, .bearer = false};
}
