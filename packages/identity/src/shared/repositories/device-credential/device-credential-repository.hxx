#pragma once

#include "device-credential-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/device-credential/device-credential-schema.hxx>
#include <string>

class DeviceCredentialRepository
{
public:
  DeviceCredentialRepository() = default;
  ~DeviceCredentialRepository() = default;

  drogon::Task<DeviceCredentialSchema>
  create(const DeviceCredentialCreateInput& input) const;

  drogon::Task<std::optional<DeviceCredentialSchema>>
  findActiveBySecretHash(const std::string& secretHash) const;
};
