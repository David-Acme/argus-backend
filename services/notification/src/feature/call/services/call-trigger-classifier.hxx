#pragma once

#include <feature/call/vocabulary/call-trigger.hxx>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

struct CallCandidate
{
  CallTrigger trigger{CallTrigger::Assistant};
  bool critical{false};
  std::string dedupeKey;
  std::string urgency;
  int64_t environmentId{0};
  std::string lang;
  Json::Value data;
};

namespace call_trigger
{
std::optional<CallCandidate> fromNotification(const Json::Value& data);
}
