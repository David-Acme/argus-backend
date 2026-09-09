#pragma once

#include "device-login-challenge-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/device-login-challenge/device-login-challenge-schema.hxx>
#include <string>

class DeviceLoginChallengeRepository
{
public:
  DeviceLoginChallengeRepository() = default;
  ~DeviceLoginChallengeRepository() = default;

  drogon::Task<DeviceLoginChallengeSchema>
  create(const DeviceLoginChallengeCreateInput& input) const;

  drogon::Task<std::optional<DeviceLoginChallengeSchema>>
  findByChallengeId(const std::string& challengeId) const;

  drogon::Task<bool>
  markApproved(const DeviceLoginChallengeMarkApprovedInput& input) const;

  drogon::Task<bool> remove(const std::string& challengeId) const;
  drogon::Task<int> deleteExpired(int64_t now) const;
};
