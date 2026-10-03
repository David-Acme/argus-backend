#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <vector>
#include <voice/voice-lang.hxx>

namespace voiceprint_challenge_query
{
inline constexpr const char* FIND_BY_TOKEN_HASH =
    "SELECT * FROM voiceprint_challenge WHERE token_hash = ?";

inline constexpr const char* INSERT =
    "INSERT INTO voiceprint_challenge "
    "(token_hash, user_id, requester_id, device_hash, lang, phrases, "
    "expires_at) VALUES (?, ?, ?, ?, ?, ?, ?)";

inline constexpr const char* TRY_CONSUME =
    "UPDATE voiceprint_challenge SET consumed_at = strftime('%s', 'now') "
    "WHERE id = ? AND consumed_at IS NULL AND expires_at > ?";

inline constexpr const char* PURGE_SAMPLES =
    "DELETE FROM voiceprint_challenge_sample WHERE challenge_id IN "
    "(SELECT id FROM voiceprint_challenge WHERE expires_at <= ?)";

inline constexpr const char* PURGE =
    "DELETE FROM voiceprint_challenge WHERE expires_at <= ?";

inline constexpr const char* STAGE_SAMPLE =
    "INSERT INTO voiceprint_challenge_sample "
    "(challenge_id, position, embedding, speech_seconds) VALUES (?, ?, ?, ?) "
    "ON CONFLICT (challenge_id, position) DO UPDATE SET "
    "embedding = excluded.embedding, speech_seconds = excluded.speech_seconds, "
    "created_at = strftime('%s', 'now')";

inline constexpr const char* FIND_SAMPLES =
    "SELECT * FROM voiceprint_challenge_sample WHERE challenge_id = ? "
    "ORDER BY position";

inline constexpr const char* DELETE_SAMPLES =
    "DELETE FROM voiceprint_challenge_sample WHERE challenge_id = ?";
}

struct VoiceprintChallengeCreateInput
{
  std::string tokenHash;
  int64_t userId{0};
  int64_t requesterId{0};
  std::string deviceHash;
  VoiceLang lang{VoiceLang::Es};
  std::string phrases;
  int64_t expiresAt{0};
};

struct VoiceprintStageSampleInput
{
  int64_t challengeId{0};
  int position{0};
  std::vector<char> embedding;
  double speechSeconds{0.0};
};

struct VoiceprintChallengeConsumeInput
{
  int64_t id{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};
