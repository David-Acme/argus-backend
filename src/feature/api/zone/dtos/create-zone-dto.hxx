#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateZoneDto
{
  int64_t cameraId{0};
  std::string name;
  // Already normalized and re-serialized by `fromJson`, so the service can
  // hand it straight to the repository.
  std::string points;
  std::string zoneType;
  std::string color;
  bool isEnabled{true};

  static CreateZoneDto fromJson(const Json::Value& json);
};
