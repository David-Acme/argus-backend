#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct CreateExpectedGuestDto
{
  std::string description;
  int64_t cameraId{0};
  int64_t environmentId{0};
  int64_t personId{0};
  int64_t hostUserId{0};
  bool oneTime{false};
  int64_t validFrom{0};
  int64_t validUntil{0};
  int hours{4};

  static CreateExpectedGuestDto fromJson(const Json::Value& json);
};
