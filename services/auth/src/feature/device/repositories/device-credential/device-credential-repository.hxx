#pragma once

#include "device-credential-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/device/schemas/device-credential/device-credential-schema.hxx>
#include <optional>
#include <string>

class DeviceCredentialRepository
{
public:
  DeviceCredentialRepository() = default;
  ~DeviceCredentialRepository() = default;

  [[nodiscard]] drogon::Task<DeviceCredentialSchema>
  create(const DeviceCredentialCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<DeviceCredentialSchema>>
  findActiveBySecretHash(const std::string& secretHash) const;
};
