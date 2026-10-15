#pragma once

#include <shared/repositories/notification-token/notification-token-repository.hxx>
#include <shared/services/push-delivery/push-device.hxx>

class NotificationPushDeviceDirectory final : public PushDeviceDirectory
{
public:
  [[nodiscard]] drogon::Task<std::vector<PushDevice>>
  devicesOf(int64_t userId, drogon::orm::DbClient* client) const override;

private:
  NotificationTokenRepository repository_;
};
