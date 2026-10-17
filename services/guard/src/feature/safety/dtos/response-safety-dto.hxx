#pragma once

#include <feature/guard/vocabulary/safety-reach.hxx>

#include <cstdint>
#include <json/value.h>

struct ResponseSafetyStatusDto
{
  bool duressEnabled{false};
  bool hasPin{false};
  bool hasPhrase{false};
  SafetyReach alertsReach{SafetyReach::Unknown};
  bool appSirenEnabled{false};

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponsePanicDto
{
  int64_t alertId{0};
  bool sent{false};
  bool repeated{false};

  [[nodiscard]] Json::Value toJson() const;
};
