#pragma once

#include "portrait-access-request-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/portrait-access-request/portrait-access-request-schema.hxx>
#include <vector>

class PortraitAccessRequestRepository
{
public:
  PortraitAccessRequestRepository() = default;

  drogon::Task<PortraitAccessRequestSchema>
  create(const PortraitAccessRequestCreateInput& input) const;
  drogon::Task<std::optional<PortraitAccessRequestSchema>>
  findById(int64_t id) const;
  drogon::Task<std::vector<PortraitAccessRequestSchema>>
  findPendingForPortraitUser(int64_t portraitUserId) const;
  drogon::Task<bool>
  resolve(const PortraitAccessRequestResolveInput& input) const;
};
