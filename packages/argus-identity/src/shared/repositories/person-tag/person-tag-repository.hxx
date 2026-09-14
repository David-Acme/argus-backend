#pragma once

#include "person-tag-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>
#include <vector>

class PersonTagRepository
{
public:
  PersonTagRepository() = default;
  ~PersonTagRepository() = default;

  drogon::Task<int> addMany(const PersonTagAddInput& input) const;

  drogon::Task<std::vector<std::string>> findByPerson(int64_t personId) const;
};
