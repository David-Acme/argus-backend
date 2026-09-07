#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <string>

struct DeviceCredentialSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string deviceHash;
  std::string secretHash;
  bool isActive{true};
  int64_t createdAt{0};

  DeviceCredentialSchema() = default;
  explicit DeviceCredentialSchema(const drogon::orm::Row& row);
};
