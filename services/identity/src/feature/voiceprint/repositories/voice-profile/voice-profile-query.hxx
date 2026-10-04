#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <feature/voiceprint/vocabulary/voice-profile-source.hxx>
#include <span>
#include <string>
#include <vector>

namespace voice_profile_query
{
inline constexpr const char* FIND_BY_USER =
    "SELECT * FROM voice_profile WHERE user_id = ?";

inline constexpr const char* FIND_BY_MODEL =
    "SELECT p.* FROM voice_profile p JOIN user u ON u.id = p.user_id "
    "WHERE p.model = ? AND u.deleted_at IS NULL ORDER BY p.user_id";

inline constexpr const char* UPSERT =
    "INSERT INTO voice_profile (user_id, model, embedding, sample_count, "
    "speech_seconds, source, linked_at, refreshed_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(user_id) DO UPDATE SET model = excluded.model, "
    "embedding = excluded.embedding, sample_count = excluded.sample_count, "
    "speech_seconds = excluded.speech_seconds, source = excluded.source, "
    "linked_at = excluded.linked_at, refreshed_at = excluded.refreshed_at "
    "RETURNING *";

inline constexpr const char* REFRESH =
    "UPDATE voice_profile SET embedding = ?, sample_count = ?, "
    "speech_seconds = ?, refreshed_at = ? WHERE id = ? RETURNING *";

inline constexpr const char* DELETE_BY_USER =
    "DELETE FROM voice_profile WHERE user_id = ? RETURNING id";

inline constexpr const char* FIND_FOR_INDEX =
    "SELECT p.id, p.user_id, p.embedding FROM voice_profile p "
    "JOIN user u ON u.id = p.user_id "
    "WHERE p.model = ? AND u.deleted_at IS NULL";

inline constexpr const char* LEGACY_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'voiceprint'";

inline constexpr const char* LEGACY_COPY =
    "INSERT OR IGNORE INTO voice_profile (user_id, model, embedding, "
    "sample_count, speech_seconds, source, linked_at, refreshed_at, "
    "created_at) SELECT user_id, model, embedding, sample_count, "
    "speech_seconds, 'enrolled', created_at, created_at, created_at "
    "FROM voiceprint";

inline constexpr const char* LEGACY_DROP_SAMPLES =
    "DROP TABLE IF EXISTS voiceprint_challenge_sample";

inline constexpr const char* LEGACY_DROP_CHALLENGES =
    "DROP TABLE IF EXISTS voiceprint_challenge";

inline constexpr const char* LEGACY_DROP_PROFILES =
    "DROP TABLE IF EXISTS voiceprint";

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

struct VoiceProfileUpsertInput
{
  int64_t userId{0};
  std::string model;
  std::vector<char> embedding;
  int sampleCount{0};
  double speechSeconds{0.0};
  VoiceProfileSource source{VoiceProfileSource::Passive};
  int64_t linkedAt{0};
  int64_t refreshedAt{0};
  drogon::orm::DbClient* client{nullptr};
};

struct VoiceProfileRefreshInput
{
  int64_t id{0};
  std::vector<char> embedding;
  int sampleCount{0};
  double speechSeconds{0.0};
  int64_t refreshedAt{0};
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
