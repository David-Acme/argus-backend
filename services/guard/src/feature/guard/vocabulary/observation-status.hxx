#pragma once

#include <cstdint>
#include <string>

enum class ObservationStatus : uint8_t
{
  Processing = 0,
  Completed,
  DeadLettered
};

inline std::string observationStatusToString(ObservationStatus status)
{
  switch (status) {
    case ObservationStatus::Processing:
      return "processing";
    case ObservationStatus::Completed:
      return "completed";
    case ObservationStatus::DeadLettered:
      return "dead_lettered";
  }
  return "processing";
}

inline ObservationStatus observationStatusFromString(const std::string& value)
{
  if (value == "completed")
    return ObservationStatus::Completed;
  if (value == "dead_lettered")
    return ObservationStatus::DeadLettered;
  return ObservationStatus::Processing;
}
