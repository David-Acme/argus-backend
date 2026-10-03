#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
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

inline constexpr const char* PURGE =
    "DELETE FROM voiceprint_challenge WHERE expires_at <= ?";
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

struct VoiceprintChallengeConsumeInput
{
  int64_t id{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};
