#pragma once

#include "visitor-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <span>
#include <vector>

class VisitorRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<VisitorRow>>
  list(const VisitorListInput& input) const;
  [[nodiscard]] drogon::Task<std::optional<VisitorRow>>
  find(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  [[nodiscard]] drogon::Task<std::vector<VisitorSampleRow>>
  samples(int64_t personId, drogon::orm::DbClient* client = nullptr) const;
  [[nodiscard]] drogon::Task<std::vector<VisitorVisitRow>>
  visits(int64_t personId, int64_t limit) const;
  [[nodiscard]] drogon::Task<std::vector<int64_t>> visitTimes(int64_t personId) const;
  drogon::Task<bool> update(int64_t id, const VisitorUpdateInput& input) const;
  [[nodiscard]] drogon::Task<std::vector<VisitorSampleOwner>>
  sampleOwners(std::span<const int64_t> sampleIds,
               drogon::orm::DbClient* client) const;
  drogon::Task<void> merge(const VisitorMergeInput& input) const;
  [[nodiscard]] drogon::Task<int64_t>
  createVisitor(drogon::orm::DbClient* client) const;
  drogon::Task<void> split(const VisitorSplitInput& input) const;
  drogon::Task<VisitorRemoved> remove(const VisitorRemoveInput& input) const;
  drogon::Task<void> refreshCounts(int64_t personId,
                                   drogon::orm::DbClient* client) const;
  drogon::Task<void> deleteSamples(std::span<const int64_t> sampleIds) const;
  drogon::Task<void> setSampleCrop(const VisitorSampleCropInput& input) const;
  [[nodiscard]] drogon::Task<std::optional<std::string>>
  sampleCrop(int64_t personId, int64_t sampleId) const;
};
