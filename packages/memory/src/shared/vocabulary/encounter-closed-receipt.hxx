#pragma once

#include <cstdint>
#include <optional>
#include <string>

// LLM-side encounter receipt lifecycle; the CHECK constraint mirrors it.
// Unknown persisted values fail closed at the call site.
enum class EncounterClosedReceipt : uint8_t
{
  Received = 0,
  Dispatched,
  Conflict,
  DeadLettered
};

inline std::string
encounterClosedReceiptToString(EncounterClosedReceipt status)
{
  switch (status) {
    case EncounterClosedReceipt::Received:
      return "received";
    case EncounterClosedReceipt::Dispatched:
      return "dispatched";
    case EncounterClosedReceipt::Conflict:
      return "conflict";
    case EncounterClosedReceipt::DeadLettered:
      return "dead_lettered";
  }
  return "received";
}

inline std::optional<EncounterClosedReceipt>
encounterClosedReceiptFromString(const std::string& value)
{
  if (value == "received")
    return EncounterClosedReceipt::Received;
  if (value == "dispatched")
    return EncounterClosedReceipt::Dispatched;
  if (value == "conflict")
    return EncounterClosedReceipt::Conflict;
  if (value == "dead_lettered")
    return EncounterClosedReceipt::DeadLettered;
  return std::nullopt;
}
