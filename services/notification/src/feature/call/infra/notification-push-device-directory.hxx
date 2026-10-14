#pragma once

#include <feature/call/services/call-ports.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>

class NotificationPushDeviceDirectory final : public CallPushDeviceDirectory
{
public:
  [[nodiscard]] drogon::Task<std::vector<CallPushDevice>>
  devicesOf(int64_t userId) const override;

private:
  NotificationTokenRepository repository_;
};
