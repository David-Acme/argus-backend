#pragma once

#include "candidate-retention-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <vector>

class CandidateRetentionRepository
{
public:
  drogon::Task<std::vector<RetiredCandidate>>
  retireStale(const CandidateRetireInput& input) const;

  drogon::Task<RetiredBiometrics>
  purgeBiometrics(const std::vector<RetiredCandidate>& retired,
                  drogon::orm::DbClient* client) const;
};
