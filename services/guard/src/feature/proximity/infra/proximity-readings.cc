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

std::string entityName(std::string_view entity, std::string_view domain)
{
  std::string_view name = trimmed(entity);
  const std::string prefix = std::string(domain) + ".";
  if (name.starts_with(prefix))
    name.remove_prefix(prefix.size());
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
  return {.path = "/text_sensor/" + entityName(entity, "text_sensor"), .bearer = false};
}

ProximityRequestPath radarRequestPath(const ProximitySensorRow& sensor)
{
  if (sensor.source == ProximitySensorSource::HomeAssistant)
    return {.path = "/api/states/" + std::string(trimmed(sensor.entityId)), .bearer = true};
  return {.path = "/binary_sensor/" + entityName(sensor.entityId, "binary_sensor"), .bearer = false};
}
