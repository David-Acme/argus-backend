#pragma once

#include <feature/safety/infra/safety-ports.hxx>

class GuardService;

class GuardAlertSink : public SafetyAlertSink
{
public:
  explicit GuardAlertSink(GuardService& guard);

  [[nodiscard]] drogon::Task<bool> raise(const SafetyAlertNotice& notice) const override;

private:
  GuardService& guard_;
};
