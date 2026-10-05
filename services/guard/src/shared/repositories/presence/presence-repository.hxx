#pragma once

#include "presence-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class PresenceRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<PresenceRow>>
  forEnvironment(const PresenceLookupInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<PresenceRow>>
  forUser(int64_t userId) const;

  [[nodiscard]] drogon::Task<std::vector<PresenceRow>> list() const;

  [[nodiscard]] drogon::Task<std::optional<PresenceRow>>
  find(const PresenceKey& key) const;

  [[nodiscard]] drogon::Task<bool> upsert(const PresenceRow& row) const;

  [[nodiscard]] drogon::Task<PresenceDecision>
  transition(const PresenceTransition& input) const;

  [[nodiscard]] drogon::Task<std::vector<int64_t>>
  removeUser(int64_t userId) const;

  [[nodiscard]] drogon::Task<std::vector<int64_t>> userIds() const;

  [[nodiscard]] drogon::Task<std::vector<int64_t>> lanEnvironments() const;

  [[nodiscard]] drogon::Task<std::vector<PresenceRow>>
  expireHome(const PresenceExpireInput& input) const;

  [[nodiscard]] drogon::Task<int64_t> purgeStale(int64_t signalBefore) const;
};
