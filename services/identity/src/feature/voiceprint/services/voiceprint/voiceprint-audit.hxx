#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/user-action.hxx>

struct VoiceprintAuditInput
{
  int64_t actorId{0};
  int64_t subjectId{0};
  UserAction action{UserAction::Create};
  Json::Value data;
  drogon::orm::DbClient* client{nullptr};
};

namespace voiceprint_audit
{
[[nodiscard]] drogon::Task<void> publish(const VoiceprintAuditInput& input);
}
