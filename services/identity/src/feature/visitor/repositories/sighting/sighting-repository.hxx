#pragma once

#include "sighting-query.hxx"

#include <drogon/orm/DbClient.h>
#include <span>
#include <unordered_map>

class SightingRepository
{
public:
  explicit SightingRepository(drogon::orm::DbClient& client) : client_(client)
  {
  }

  [[nodiscard]] std::unordered_map<int64_t, SightingPerson>
  findPersons(std::span<const int64_t> ids) const;
  [[nodiscard]] std::vector<SightingSample> findSamples(int64_t personId) const;
  [[nodiscard]] SightingPerson createVisitor(int64_t at) const;
  [[nodiscard]] int64_t insertSample(const SightingSampleInsertInput& input) const;
  void deleteSample(int64_t sampleId) const;
  void setSampleCrop(int64_t sampleId, const std::string& cropKey) const;
  [[nodiscard]] bool recordVisit(const SightingVisitInput& input) const;

private:
  drogon::orm::DbClient& client_;
};
