#pragma once

#include <feature/safety/infra/safety-ports.hxx>

class GuardService;

class GuardAlertSink : public SafetyAlertSink
{
public:
  explicit GuardAlertSink(GuardService& guard);

  [[nodiscard]] drogon::Task<SafetyDelivery> raise(const SafetyAlertNotice& notice) const override;

private:
  GuardService& guard_;
};
