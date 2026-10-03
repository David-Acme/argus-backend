#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <string>
#include <voice/voice-lang.hxx>

struct VoiceprintChallengeSchema
{
  int64_t id{0};
  std::string tokenHash;
  int64_t userId{0};
  int64_t requesterId{0};
  std::string deviceHash;
  VoiceLang lang{VoiceLang::Es};
  std::string phrases;
  int64_t expiresAt{0};
  std::optional<int64_t> consumedAt;
  int64_t createdAt{0};

  VoiceprintChallengeSchema() = default;
  explicit VoiceprintChallengeSchema(const drogon::orm::Row& row);
};
