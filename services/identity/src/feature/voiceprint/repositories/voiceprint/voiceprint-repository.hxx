#pragma once

#include "voiceprint-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/schemas/voiceprint/voiceprint-schema.hxx>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

class VoiceprintRepository
{
public:
  [[nodiscard]] drogon::Task<std::optional<VoiceprintSchema>>
  findByUser(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<VoiceprintSchema>
  create(const VoiceprintCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<int64_t>>
  removeByUser(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] std::vector<VoiceprintIndexRow>
  findForModel(sqlite3* db, const std::string& model) const;

  [[nodiscard]] bool ensureVecTable(sqlite3* db, int dims) const;

  [[nodiscard]] bool clearVec(sqlite3* db) const;

  [[nodiscard]] bool insertVec(sqlite3* db,
                               const VoiceprintVecEntry& entry) const;

  [[nodiscard]] bool deleteVec(sqlite3* db, int64_t voiceprintId) const;

  [[nodiscard]] std::vector<VoiceprintVecHit>
  searchVec(sqlite3* db, const VoiceprintVecSearchInput& input) const;
};
