#pragma once

#include "portrait-access-grant-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/portrait-access-grant/portrait-access-grant-schema.hxx>
#include <vector>

class PortraitAccessGrantRepository
{
public:
  PortraitAccessGrantRepository() = default;

  drogon::Task<PortraitAccessGrantSchema>
  create(const PortraitAccessGrantCreateInput& input) const;
  drogon::Task<std::optional<PortraitAccessGrantSchema>>
  findById(int64_t id) const;
  drogon::Task<std::optional<PortraitAccessGrantSchema>>
  findActive(const PortraitAccessGrantFindActiveInput& input) const;
  drogon::Task<std::vector<PortraitAccessGrantSchema>>
  findActiveForGrantee(int64_t granteeUserId, int64_t now) const;
  drogon::Task<bool>
  revoke(const PortraitAccessGrantRevokeInput& input) const;
};
