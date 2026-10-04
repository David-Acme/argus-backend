#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <string>

struct VoiceDeviceSchema
{
  std::string deviceHash;
  int64_t userId{0};
  int calls{0};
  int matched{0};
  int conflicting{0};
  int mixed{0};
  int64_t lastCallAt{0};

  VoiceDeviceSchema() = default;
  explicit VoiceDeviceSchema(const drogon::orm::Row& row);
};
