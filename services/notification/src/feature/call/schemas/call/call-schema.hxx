#pragma once

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <feature/call/vocabulary/call-state.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>
#include <json/value.h>

#include <cstdint>
#include <string>

struct CallSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string dedupeKey;
  CallTrigger trigger{CallTrigger::Assistant};
  CallState state{CallState::Ringing};
  std::string reason;
  int64_t parentCallId{0};
  std::string urgency;
  std::string lang;
  std::string title;
  std::string summary;
  std::string openingLine;
  std::string missedLine;
  Json::Value data;
  std::string answeredSession;
  int64_t createdAt{0};
  int64_t expiresAt{0};
  int64_t pushedAt{0};
  int64_t answeredAt{0};
  int64_t endedAt{0};

  [[nodiscard]] std::string callId() const;

  static CallSchema fromRow(const drogon::orm::Row& row);
};

namespace call_id
{
std::string format(int64_t id);

int64_t parse(const std::string& callId);
}
