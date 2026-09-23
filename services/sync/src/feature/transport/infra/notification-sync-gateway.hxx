#pragma once

#include <memory>
#include <notification/notification-client.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>

class NotificationSyncGateway : public NotificationSyncSource
{
public:
  explicit NotificationSyncGateway(std::string target);

  NotificationSyncGateway(const NotificationSyncGateway&) = delete;
  NotificationSyncGateway& operator=(const NotificationSyncGateway&) = delete;

  [[nodiscard]] drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter& filter) const override;

  [[nodiscard]] drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext& ctx) const override;

private:
  std::shared_ptr<NotificationClient> client_;
};
