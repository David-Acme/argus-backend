#pragma once

#include "voiceprint-challenge-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/schemas/voiceprint-challenge/voiceprint-challenge-schema.hxx>
#include <optional>
#include <string>

class VoiceprintChallengeRepository
{
public:
  [[nodiscard]] drogon::Task<VoiceprintChallengeSchema>
  create(const VoiceprintChallengeCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<VoiceprintChallengeSchema>>
  findByTokenHash(const std::string& tokenHash,
                  drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<bool>
  tryConsume(const VoiceprintChallengeConsumeInput& input) const;

  drogon::Task<void> purgeExpired(int64_t now) const;
};
