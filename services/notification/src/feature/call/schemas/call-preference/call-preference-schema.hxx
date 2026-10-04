#pragma once

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <feature/call/vocabulary/call-mode.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>
#include <json/value.h>

#include <cstdint>
#include <vector>

struct CallPreferenceSchema
{
  int64_t userId{0};
  bool enabled{true};
  CallMode guardCritical{CallMode::Call};
  CallMode guardIntruder{CallMode::Call};
  CallMode guardEscalation{CallMode::Call};
  CallMode guardArrival{CallMode::Off};
  CallMode agenda{CallMode::Call};
  CallMode assistant{CallMode::Call};
  int quietStartHour{-1};
  int quietEndHour{-1};
  int64_t dndUntil{0};
  bool criticalBypass{true};
  std::vector<int64_t> mutedEnvironmentIds;
  int64_t updatedAt{0};

  [[nodiscard]] CallMode modeFor(CallTrigger trigger) const;
  [[nodiscard]] bool mutes(int64_t environmentId) const;
  [[nodiscard]] Json::Value toJson() const;

  static CallPreferenceSchema defaultsFor(int64_t userId);
  static CallPreferenceSchema fromRow(const drogon::orm::Row& row);
};
