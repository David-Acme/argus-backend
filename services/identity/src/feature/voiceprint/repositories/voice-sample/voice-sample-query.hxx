#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <feature/voiceprint/vocabulary/voice-sample-state.hxx>
#include <string>
#include <vector>

namespace voice_sample_query
{
inline constexpr const char* INSERT =
    "INSERT INTO voice_sample (user_id, model, device_hash, embedding, turns, "
    "speech_seconds, state, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr const char* FIND_SINCE =
    "SELECT * FROM voice_sample WHERE user_id = ? AND model = ? "
    "AND created_at >= ? ORDER BY created_at, id";

inline constexpr const char* FIND_ADOPTED =
    "SELECT * FROM voice_sample WHERE user_id = ? AND model = ? "
    "AND state = 'adopted' ORDER BY created_at DESC, id DESC LIMIT ?";

inline constexpr const char* COUNT_ADOPTED_SINCE =
    "SELECT COUNT(*) AS total FROM voice_sample WHERE user_id = ? "
    "AND model = ? AND state = 'adopted' AND created_at > ?";

inline constexpr const char* ADOPT_PREFIX =
    "UPDATE voice_sample SET state = 'adopted' WHERE user_id = ? AND id IN (";

inline constexpr const char* DROP_ADOPTED_PREFIX =
    "DELETE FROM voice_sample WHERE user_id = ? AND state = 'adopted' "
    "AND id NOT IN (";

inline constexpr const char* PRUNE_OTHER_MODELS =
    "DELETE FROM voice_sample WHERE user_id = ? AND model <> ?";

inline constexpr const char* PRUNE_OVERFLOW =
    "DELETE FROM voice_sample WHERE user_id = ? AND model = ? AND state = ? "
    "AND id NOT IN (SELECT id FROM voice_sample WHERE user_id = ? "
    "AND model = ? AND state = ? ORDER BY created_at DESC, id DESC LIMIT ?)";

inline constexpr const char* PURGE_PENDING_BEFORE =
    "DELETE FROM voice_sample WHERE state = 'pending' AND created_at < ?";

inline constexpr const char* PURGE_ADOPTED_BEFORE =
    "DELETE FROM voice_sample WHERE state = 'adopted' AND created_at < ?";

inline constexpr const char* DELETE_BY_USER =
    "DELETE FROM voice_sample WHERE user_id = ?";
}

struct VoiceSampleCreateInput
{
  int64_t userId{0};
  std::string model;
  std::string deviceHash;
  std::vector<char> embedding;
  int turns{0};
  double speechSeconds{0.0};
  VoiceSampleState state{VoiceSampleState::Pending};
  int64_t createdAt{0};
  drogon::orm::DbClient* client{nullptr};
};

struct VoiceSampleWindowInput
{
  int64_t userId{0};
  std::string model;
  int64_t since{0};
};

struct VoiceSampleAdoptedInput
{
  int64_t userId{0};
  std::string model;
  int limit{0};
};

struct VoiceSampleCountInput
{
  int64_t userId{0};
  std::string model;
  int64_t after{0};
};

struct VoiceSampleAdoptInput
{
  int64_t userId{0};
  std::vector<int64_t> ids;
  drogon::orm::DbClient* client{nullptr};
};

struct VoiceSamplePruneInput
{
  int64_t userId{0};
  std::string model;
  int maxPending{0};
  int maxAdopted{0};
  int64_t pendingBefore{0};
  int64_t adoptedBefore{0};
};
