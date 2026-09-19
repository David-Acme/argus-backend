#pragma once

#include <cstdint>
#include <string>

// How far a person row is trusted: auto-enrolled sightings are candidates only.
enum class PersonStatus : uint8_t
{
  Candidate = 0,
  Known
};

inline std::string personStatusToString(PersonStatus status)
{
  return status == PersonStatus::Known ? "known" : "candidate";
}

inline PersonStatus personStatusFromString(const std::string& value)
{
  return value == "known" ? PersonStatus::Known : PersonStatus::Candidate;
}
