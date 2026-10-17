#pragma once

#include <feature/guard/vocabulary/safety-reach.hxx>
#include <shared/vocabulary/masked-camera.hxx>

#include <cstdint>
#include <json/value.h>
#include <vector>

struct ResponseSafetyStatusDto
{
  bool duressEnabled{false};
  bool hasPin{false};
  bool hasPhrase{false};
  SafetyReach alertsReach{SafetyReach::Unknown};
  bool appSirenEnabled{false};
  std::vector<MaskedCamera> maskedCameras{};

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponsePanicDto
{
  int64_t alertId{0};
  bool sent{false};
  bool repeated{false};

  [[nodiscard]] Json::Value toJson() const;
};
