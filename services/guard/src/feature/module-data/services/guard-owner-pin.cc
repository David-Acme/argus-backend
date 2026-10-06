#include "guard-owner-pin.hxx"

#include <drogon/utils/coroutine.h>

#include <optional>
#include <stdexcept>
#include <utility>

GuardOwnerPin::GuardOwnerPin(std::shared_ptr<const SafetyService> safety) : safety_(std::move(safety))
{
  if (!safety_)
    throw std::invalid_argument("GuardOwnerPin needs the safety service");
}

PinVerdict GuardOwnerPin::verdictOf(OwnerPinOutcome outcome)
{
  switch (outcome) {
  case OwnerPinOutcome::NoPin: return PinVerdict::NoPin;
  case OwnerPinOutcome::Accepted: return PinVerdict::Accepted;
  case OwnerPinOutcome::Required: return PinVerdict::Required;
  case OwnerPinOutcome::Invalid: return PinVerdict::Invalid;
  case OwnerPinOutcome::Locked: return PinVerdict::Locked;
  }
  return PinVerdict::Invalid;
}

PinVerdict GuardOwnerPin::verify(const OwnerPinCheck& check)
{
  const auto outcome = drogon::sync_wait(safety_->verifyOwnerPin(
      {.userId = check.userId, .userName = {}, .pin = check.pin, .environmentId = std::nullopt}));
  return verdictOf(outcome);
}
