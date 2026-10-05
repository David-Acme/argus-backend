#pragma once

#include <json/value.h>

#include <string>

struct ResponseNotice
{
  std::string title;
  std::string body;
};

struct ResponseEscalationInput
{
  std::string lang;
  std::string summary;
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
