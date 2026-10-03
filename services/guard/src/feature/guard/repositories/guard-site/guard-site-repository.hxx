#pragma once

#include "guard-site-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>

class GuardSiteRepository
{
public:
  [[nodiscard]] drogon::Task<std::optional<GuardSite>> find() const;

  [[nodiscard]] drogon::Task<GuardSite> update(const GuardSiteUpdateInput& input) const;
};
