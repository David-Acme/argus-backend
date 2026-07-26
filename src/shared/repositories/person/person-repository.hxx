#pragma once

#include "person-query.hxx"
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/person/person-schema.hxx>

class PersonRepository
{
public:
  PersonRepository() = default;
  ~PersonRepository() = default;

  drogon::Task<std::optional<PersonSchema>> findById(int64_t id) const;
  drogon::Task<std::optional<PersonSchema>>
  findByFeatureHub(int64_t featureHubId) const;
  drogon::Task<PersonSchema> create(const PersonCreateInput& input) const;
  drogon::Task<PersonSchema> update(int64_t id,
                                    const PersonUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;
};
