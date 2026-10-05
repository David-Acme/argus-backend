#pragma once

#include <feature/safety/infra/safety-ports.hxx>

class IdentityClient;
class NotificationClient;

class NotificationActorNotifier : public SafetyActorNotifier
{
public:
  struct Dependencies
  {
    const NotificationClient* notifications{nullptr};
    const IdentityClient* identity{nullptr};
  };

  explicit NotificationActorNotifier(Dependencies dependencies);

  [[nodiscard]] drogon::Task<bool> confirmPanic(const SafetyAlertNotice& notice) const override;

private:
  Dependencies dependencies_;
};
