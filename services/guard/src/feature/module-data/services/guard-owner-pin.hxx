#pragma once

#include <feature/safety/services/safety-service.hxx>

#include <settings/owner-pin-host.hxx>

#include <memory>

class GuardOwnerPin final : public OwnerPinHost
{
public:
  explicit GuardOwnerPin(std::shared_ptr<const SafetyService> safety);
  ~GuardOwnerPin() override = default;
  GuardOwnerPin(const GuardOwnerPin&) = delete;
  GuardOwnerPin& operator=(const GuardOwnerPin&) = delete;
  GuardOwnerPin(GuardOwnerPin&&) = delete;
  GuardOwnerPin& operator=(GuardOwnerPin&&) = delete;

  [[nodiscard]] static PinVerdict verdictOf(OwnerPinOutcome outcome);

  PinVerdict verify(const OwnerPinCheck& check) override;

private:
  std::shared_ptr<const SafetyService> safety_;
};
