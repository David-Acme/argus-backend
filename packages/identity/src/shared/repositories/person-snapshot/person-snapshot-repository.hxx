#pragma once

#include "person-snapshot-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

class PersonSnapshotRepository
{
public:
  PersonSnapshotRepository() = default;
  ~PersonSnapshotRepository() = default;

  drogon::Task<bool> store(const PersonSnapshotStoreInput& input) const;

  drogon::Task<std::optional<std::string>> findImage(int64_t personId) const;
};
