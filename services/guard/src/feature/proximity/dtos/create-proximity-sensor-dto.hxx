#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

struct CreateProximitySensorDto
{
  std::string name;
  std::string room;
  std::string model;
  std::string source;
  std::string host;
  int64_t port{80};
  std::string entityId;
  std::optional<int64_t> cameraId;
  std::optional<double> panOffsetDeg;

  static CreateProximitySensorDto fromJson(const Json::Value& json);
};
