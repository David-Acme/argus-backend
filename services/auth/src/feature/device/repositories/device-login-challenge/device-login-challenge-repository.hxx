#pragma once

#include "device-login-challenge-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/device/schemas/device-login-challenge/device-login-challenge-schema.hxx>
#include <optional>
#include <string>

class DeviceLoginChallengeRepository
{
public:
  DeviceLoginChallengeRepository() = default;
  ~DeviceLoginChallengeRepository() = default;

  [[nodiscard]] bool migrateLegacySchema() const;

  [[nodiscard]] drogon::Task<DeviceLoginChallengeSchema>
  create(const DeviceLoginChallengeCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<DeviceLoginChallengeSchema>>
  findByChallengeId(const std::string& challengeId) const;

  [[nodiscard]] drogon::Task<bool>
  markApproved(const DeviceLoginChallengeMarkApprovedInput& input) const;

  [[nodiscard]] drogon::Task<bool>
  claimApproved(const std::string& challengeId, int64_t now) const;

  [[nodiscard]] drogon::Task<bool> remove(const std::string& challengeId) const;

  [[nodiscard]] drogon::Task<int64_t> removeExpired(int64_t now) const;
};
