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
