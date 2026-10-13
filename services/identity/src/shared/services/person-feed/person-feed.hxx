#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/person/person-schema.hxx>

struct PersonFeedAddInput
{
  PersonSchema person;
  drogon::orm::DbClient* client{nullptr};
};

struct PersonFeedChangeInput
{
  PersonSchema before;
  PersonSchema after;
  std::optional<int64_t> actorId;
  drogon::orm::DbClient* client{nullptr};
};

class PersonFeed
{
public:
  [[nodiscard]] drogon::Task<void> added(const PersonFeedAddInput& input) const;
  [[nodiscard]] drogon::Task<void> changed(const PersonFeedChangeInput& input) const;
};
