#pragma once

#include <feature/proximity/services/proximity-ports.hxx>

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ProximityRequestPath
{
  std::string path;
  bool bearer{false};
};

[[nodiscard]] std::vector<BleHeard> bleHeardOf(const Json::Value& body);

[[nodiscard]] std::optional<bool> radarPresenceOf(const Json::Value& body);

[[nodiscard]] ProximityRequestPath bleRequestPath(std::string_view entity);

[[nodiscard]] ProximityRequestPath radarRequestPath(const ProximitySensorRow& sensor);

[[nodiscard]] std::optional<std::vector<RadarPoint>> radarTargetsOf(const Json::Value& body);

struct RadarTargetsPathInput
{
  const ProximitySensorRow& sensor;
  std::string_view suffix;
};

[[nodiscard]] ProximityRequestPath radarTargetsPath(const RadarTargetsPathInput& input);
