#pragma once

#include <auth/user-role.hxx>

#include <cstdint>
#include <json/value.h>
#include <string>

namespace tools
{

struct IntentOffer
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string module;
  std::string tool;
  Json::Value arguments;
  std::string lang;
  std::string utterance;
  std::string sessionId;
};

struct ModuleAcceptance
{
  int64_t userId{0};
  std::string module;
};

class IntentLedger
{
public:
  IntentLedger() = default;
  virtual ~IntentLedger() = default;
  IntentLedger(const IntentLedger&) = delete;
  IntentLedger& operator=(const IntentLedger&) = delete;

  virtual void offered(const IntentOffer& offer) = 0;
  virtual void accepted(const ModuleAcceptance& acceptance) = 0;
};

}
