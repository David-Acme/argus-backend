#pragma once

#include "voice-device-query.hxx"

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/schemas/voice-device/voice-device-schema.hxx>
#include <string>
#include <unordered_set>

class VoiceDeviceRepository
{
public:
  [[nodiscard]] drogon::Task<VoiceDeviceSchema>
  record(const VoiceDeviceRecordInput& input) const;

  [[nodiscard]] drogon::Task<std::unordered_set<std::string>>
  sharedFor(int64_t userId) const;

  [[nodiscard]] drogon::Task<int> otherUsersOn(const std::string& deviceHash,
                                             int64_t userId) const;

  [[nodiscard]] drogon::Task<size_t>
  removeByUser(int64_t userId, drogon::orm::DbClient* client = nullptr) const;
};
