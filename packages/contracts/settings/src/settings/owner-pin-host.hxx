#pragma once

#include <settings/component-vocabulary.hxx>

#include <cstdint>
#include <string>

struct OwnerPinCheck
{
  std::int64_t userId{0};
  std::string pin;
};

class OwnerPinHost
{
public:
  OwnerPinHost() = default;
  virtual ~OwnerPinHost() = default;
  OwnerPinHost(const OwnerPinHost&) = delete;
  OwnerPinHost& operator=(const OwnerPinHost&) = delete;
  OwnerPinHost(OwnerPinHost&&) = delete;
  OwnerPinHost& operator=(OwnerPinHost&&) = delete;

  virtual PinVerdict verify(const OwnerPinCheck& check) = 0;
};
