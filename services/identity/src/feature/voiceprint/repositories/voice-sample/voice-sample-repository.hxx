#pragma once

#include "voice-sample-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/schemas/voice-sample/voice-sample-schema.hxx>
#include <vector>

class VoiceSampleRepository
{
public:
  [[nodiscard]] drogon::Task<int64_t>
  create(const VoiceSampleCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<VoiceSampleSchema>>
  findSince(const VoiceSampleWindowInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<VoiceSampleSchema>>
  findAdopted(const VoiceSampleAdoptedInput& input) const;

  [[nodiscard]] drogon::Task<int>
  countAdoptedAfter(const VoiceSampleCountInput& input) const;

  [[nodiscard]] drogon::Task<void> adopt(const VoiceSampleAdoptInput& input) const;

  [[nodiscard]] drogon::Task<void>
  dropAdoptedExcept(const VoiceSampleAdoptInput& input) const;

  [[nodiscard]] drogon::Task<void> prune(const VoiceSamplePruneInput& input) const;

  [[nodiscard]] drogon::Task<void> purgeBefore(int64_t pendingBefore,
                                               int64_t adoptedBefore) const;

  [[nodiscard]] drogon::Task<size_t>
  removeByUser(int64_t userId, drogon::orm::DbClient* client = nullptr) const;
};
