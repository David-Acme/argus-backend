#pragma once

#include <memory>
#include <notification/notification-client.hxx>
#include <shared/contracts/notification-sync-source.hxx>

// User-scoped pull source backed by argus-notification's gRPC leg.
class NotificationSyncGateway : public NotificationSyncSource
{
public:
  explicit NotificationSyncGateway(std::string target);

  NotificationSyncGateway(const NotificationSyncGateway&) = delete;
  NotificationSyncGateway& operator=(const NotificationSyncGateway&) = delete;

  drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter& filter) const override;

  drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext& ctx) const override;

private:
  std::shared_ptr<NotificationClient> client_;
};
