#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

enum class GuardIntentStatus : uint8_t
{
  Pending = 0,
  InFlight,
  RetryableFailed,
  Succeeded,
  DuplicateSucceeded,
  Rejected,
  Conflict,
  Indeterminate
};

inline std::string guardIntentStatusToString(GuardIntentStatus status)
{
  switch (status) {
    case GuardIntentStatus::Pending:
      return "pending";
    case GuardIntentStatus::InFlight:
      return "in_flight";
    case GuardIntentStatus::RetryableFailed:
      return "retryable_failed";
    case GuardIntentStatus::Succeeded:
      return "succeeded";
    case GuardIntentStatus::DuplicateSucceeded:
      return "duplicate_succeeded";
    case GuardIntentStatus::Rejected:
      return "rejected";
    case GuardIntentStatus::Conflict:
      return "conflict";
    case GuardIntentStatus::Indeterminate:
      return "indeterminate";
  }
  return "pending";
}

inline std::optional<GuardIntentStatus>
guardIntentStatusFromString(const std::string& value)
{
  static const std::unordered_map<std::string, GuardIntentStatus> kMap = {
      {"pending", GuardIntentStatus::Pending},
      {"in_flight", GuardIntentStatus::InFlight},
      {"retryable_failed", GuardIntentStatus::RetryableFailed},
      {"succeeded", GuardIntentStatus::Succeeded},
      {"duplicate_succeeded", GuardIntentStatus::DuplicateSucceeded},
      {"rejected", GuardIntentStatus::Rejected},
      {"conflict", GuardIntentStatus::Conflict},
      {"indeterminate", GuardIntentStatus::Indeterminate},
  };
  const auto found = kMap.find(value);
  if (found == kMap.end())
    return std::nullopt;
  return found->second;
}

inline bool guardIntentStatusIsResumable(GuardIntentStatus status)
{
  return status == GuardIntentStatus::Pending ||
         status == GuardIntentStatus::InFlight ||
         status == GuardIntentStatus::RetryableFailed;
}

inline bool guardIntentStatusIsTerminal(GuardIntentStatus status)
{
  return !guardIntentStatusIsResumable(status);
}
