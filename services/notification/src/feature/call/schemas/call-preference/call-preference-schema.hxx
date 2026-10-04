#pragma once

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <feature/call/vocabulary/call-mode.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>
#include <json/value.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace call_preference_bounds
{
inline constexpr std::array<int, 6> kAgendaLeads{0, 5, 10, 15, 30, 60};
inline constexpr int kMinRingSeconds = 20;
inline constexpr int kMaxRingSeconds = 90;
inline constexpr int kMaxPushDelaySeconds = 30;
inline constexpr int kAllDays = 0x7F;
}

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
  int agendaLeadMinutes{10};
  int quietDays{0x7F};
  int ringSeconds{45};
  int pushDelaySeconds{4};
  bool liveAnnounce{true};
  std::string lang;

  [[nodiscard]] CallMode modeFor(CallTrigger trigger) const;
  [[nodiscard]] bool mutes(int64_t environmentId) const;
  [[nodiscard]] int ringSecondsClamped() const;
  [[nodiscard]] int pushDelayClamped() const;
  [[nodiscard]] Json::Value toJson() const;

  static CallPreferenceSchema defaultsFor(int64_t userId);
  static CallPreferenceSchema fromRow(const drogon::orm::Row& row);
};
