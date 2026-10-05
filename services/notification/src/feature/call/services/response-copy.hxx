#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>

struct ResponseNotice
{
  std::string title;
  std::string body;
};

enum class ResponseReach : uint8_t
{
  NextStep = 0,
  Worse,
  Confirmed
};

struct ResponseEscalationInput
{
  std::string lang;
  std::string summary;
  ResponseReach reason{ResponseReach::NextStep};
  std::string confirmedBy;
};

struct ResponseContactsInput
{
  std::string lang;
  std::string place;
  const Json::Value& contacts;
  std::string emergencyNumber;
  bool confirmed{false};
  std::string confirmedBy;
};

namespace response_copy
{
std::string escalationBody(const ResponseEscalationInput& input);

ResponseNotice contacts(const ResponseContactsInput& input);
}
