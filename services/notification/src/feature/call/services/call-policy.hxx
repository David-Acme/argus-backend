#pragma once

#include <feature/call/schemas/call-preference/call-preference-schema.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>

#include <cstdint>
#include <string>

enum class CallDecision : uint8_t
{
  Ring = 0,
  Followup,
  Notify,
  Drop
};

std::string callDecisionToString(CallDecision decision);

struct CallPolicyLimits
{
  bool enabled{true};
  int64_t callGapS{300};
  int maxCallsPerHour{4};
};

struct CallPolicyInput
{
  CallTrigger trigger{CallTrigger::Assistant};
  bool critical{false};
  const CallPreferenceSchema& preference;
  int localHour{0};
  int localWeekday{0};
  int64_t now{0};
  int64_t environmentId{0};
  bool alreadyCalled{false};
  bool ringing{false};
  int64_t lastCallAt{0};
  int callsLastHour{0};
  CallPolicyLimits limits;
};

struct CallVerdict
{
  CallDecision decision{CallDecision::Notify};
  std::string reason;
  bool injectable{false};
};

namespace call_policy
{
struct QuietWindowInput
{
  int hour{0};
  int weekday{0};
  int startHour{-1};
  int endHour{-1};
  int days{0x7F};
};

bool inQuietHours(const QuietWindowInput& input);

CallVerdict decide(const CallPolicyInput& input);
}
