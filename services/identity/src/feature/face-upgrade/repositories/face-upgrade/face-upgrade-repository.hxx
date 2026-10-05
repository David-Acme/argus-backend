#pragma once

#include "face-upgrade-query.hxx"

#include <drogon/utils/coroutine.h>
#include <string_view>
#include <vector>

class FaceUpgradeRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<FaceUpgradeCandidate>>
  findStaleUserPersons(std::string_view model) const;
};
