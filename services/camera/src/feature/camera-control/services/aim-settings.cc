#include "aim-settings.hxx"

#include <text/json-util.hxx>

#include <algorithm>

namespace
{
std::vector<NoAimSector> sectorsOf(const Json::Value& list)
{
  std::vector<NoAimSector> sectors;
  if (!list.isArray())
    return sectors;
  for (const auto& entry : list) {
    if (!entry.isObject())
      continue;
    sectors.push_back({.name = entry.get("name", "").asString(),
                       .panFrom = entry.get("panFrom", 0.0).asDouble(),
                       .panTo = entry.get("panTo", 0.0).asDouble(),
                       .tiltFrom = entry.get("tiltFrom", 0.0).asDouble(),
                       .tiltTo = entry.get("tiltTo", 0.0).asDouble()});
  }
  return sectors;
}

bool flag(const Json::Value& config, const char* key, bool fallback)
{
  return config.isObject() && config[key].isBool() ? config[key].asBool() : fallback;
}
}

namespace aim_settings
{
AimSettings parse(const AimSource& source)
{
  AimSettings settings;
  const Json::Value capabilities = json_util::fromString(source.capabilities);
  if (capabilities.isArray())
    settings.ptz = std::ranges::any_of(capabilities, [](const Json::Value& name) {
      return name.isString() && name.asString() == "ptz";
    });
  const Json::Value config = json_util::fromString(source.config);
  if (!config.isObject())
    return settings;
  if (config["homePreset"].isString())
    settings.homePreset = config["homePreset"].asString();
  settings.activeLook = flag(config, "activeLook", true);
  settings.followIntruders = flag(config, "followIntruders", true);
  settings.noAimSectors = sectorsOf(config["noAimSectors"]);
  return settings;
}

Json::Value sectorsToJson(const std::vector<NoAimSector>& sectors)
{
  Json::Value list(Json::arrayValue);
  for (const NoAimSector& sector : sectors) {
    Json::Value entry(Json::objectValue);
    entry["name"] = sector.name;
    entry["panFrom"] = sector.panFrom;
    entry["panTo"] = sector.panTo;
    entry["tiltFrom"] = sector.tiltFrom;
    entry["tiltTo"] = sector.tiltTo;
    list.append(entry);
  }
  return list;
}

std::string merged(const AimMerge& input)
{
  Json::Value config = json_util::fromString(input.config);
  if (!config.isObject())
    config = Json::Value(Json::objectValue);
  if (input.patch.activeLook)
    config["activeLook"] = *input.patch.activeLook;
  if (input.patch.followIntruders)
    config["followIntruders"] = *input.patch.followIntruders;
  if (input.patch.noAimSectors)
    config["noAimSectors"] = sectorsToJson(*input.patch.noAimSectors);
  return json_util::toString(config);
}

Json::Value toJson(const AimSettings& settings)
{
  Json::Value json(Json::objectValue);
  json["activeLook"] = settings.activeLook;
  json["followIntruders"] = settings.followIntruders;
  json["noAimSectors"] = sectorsToJson(settings.noAimSectors);
  return json;
}
}
