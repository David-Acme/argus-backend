#pragma once

#include <drogon/utils/coroutine.h>
#include <identity/identity-client.hxx>
#include <settings/module-request-host.hxx>
#include <shared/services/notification/notification-service.hxx>

#include <memory>

struct NotificationModuleRequestInput
{
  std::shared_ptr<IdentityClient> identity;
  NotificationService::Dependencies delivery;
};

class NotificationModuleRequest final : public ModuleRequestHost
{
public:
  explicit NotificationModuleRequest(NotificationModuleRequestInput input);

  ModuleRequestOutcome request(const ModuleRequestInput& input) override;
  [[nodiscard]] drogon::Task<ModuleRequestOutcome> requestAsync(ModuleRequestInput input) const;

private:
  std::shared_ptr<IdentityClient> identity_;
  NotificationService service_;
};
