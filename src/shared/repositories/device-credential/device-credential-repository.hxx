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

  // Credential rows live in the identity database: writes go to the default
  // client (identity.db on the gateway, argus.db on the monolith) and the
  // filter-side lookups resolve through the same client the JwtFilter's
  // session reads use.
  drogon::Task<DeviceCredentialSchema>
  create(const DeviceCredentialCreateInput& input) const;

  drogon::Task<std::optional<DeviceCredentialSchema>>
  findActiveBySecretHash(const std::string& secretHash) const;
};
