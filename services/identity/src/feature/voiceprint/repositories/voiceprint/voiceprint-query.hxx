#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <feature/voiceprint/vocabulary/voiceprint-method.hxx>
#include <span>
#include <string>
#include <vector>

namespace voiceprint_query
{
inline constexpr const char* FIND_BY_USER =
    "SELECT * FROM voiceprint WHERE user_id = ?";

inline constexpr const char* INSERT =
    "INSERT INTO voiceprint (user_id, model, embedding, sample_count, "
    "speech_seconds, method, consent_version, enrolled_by) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr const char* DELETE_BY_USER =
    "DELETE FROM voiceprint WHERE user_id = ? RETURNING id";

inline constexpr const char* FIND_FOR_MODEL =
    "SELECT v.id, v.user_id, v.embedding FROM voiceprint v "
    "JOIN user u ON u.id = v.user_id "
    "WHERE v.model = ? AND u.deleted_at IS NULL";

inline constexpr const char* VEC_TABLE_SQL =
    "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'voice_vec'";

inline constexpr const char* VEC_DROP = "DROP TABLE IF EXISTS voice_vec";

inline std::string vecTableDdl(int dims)
{
  return "CREATE VIRTUAL TABLE IF NOT EXISTS voice_vec USING vec0("
         "embedding float[" +
         std::to_string(dims) + "] distance_metric=cosine, user_id INTEGER)";
}

inline constexpr const char* VEC_CLEAR = "DELETE FROM voice_vec";

inline constexpr const char* VEC_INSERT =
    "INSERT INTO voice_vec (rowid, embedding, user_id) VALUES (?, ?, ?)";

inline constexpr const char* VEC_DELETE =
    "DELETE FROM voice_vec WHERE rowid = ?";

inline constexpr const char* VEC_SEARCH =
    "SELECT rowid, user_id, distance FROM voice_vec "
    "WHERE embedding MATCH ? ORDER BY distance LIMIT ?";
}

struct VoiceprintCreateInput
{
  int64_t userId{0};
  std::string model;
  std::vector<char> embedding;
  int sampleCount{0};
  double speechSeconds{0.0};
  VoiceprintMethod method{VoiceprintMethod::Self};
  std::string consentVersion;
  int64_t enrolledBy{0};
  drogon::orm::DbClient* client{nullptr};
};

struct VoiceprintVecEntry
{
  int64_t voiceprintId{0};
  int64_t userId{0};
  std::span<const float> embedding;
};

struct VoiceprintVecSearchInput
{
  std::span<const float> query;
  int count{1};
};

struct VoiceprintVecHit
{
  int64_t voiceprintId{0};
  int64_t userId{0};
  float distance{0.0F};
};

struct VoiceprintIndexRow
{
  int64_t voiceprintId{0};
  int64_t userId{0};
  std::vector<float> embedding;
};
