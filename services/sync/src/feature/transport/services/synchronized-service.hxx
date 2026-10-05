#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/transport/dtos/synchronized-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/identity-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <sync/sync-filter.hxx>
#include <sync/syncable.hxx>
#include <sync/table-name.hxx>
#include <sync/socket-emit-dto.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <auth/user-role.hxx>
#include <optional>
#include <vector>

struct SyncWithRepoInput
{
  const Syncable& repo;
  const SynchronizedBodyDto& dto;
};

struct TablePullInput
{
  TableName table{TableName::User};
  const SynchronizedBodyDto& dto;
  const JwtContext& ctx;
};

class SynchronizedService
{
public:
  SynchronizedService() = default;

  void setCameraSource(const CameraSyncSource* source)
  {
    cameraSyncSource_ = source;
  }

  void setProductivitySource(const ProductivitySyncSource* source)
  {
    productivitySyncSource_ = source;
  }

  void setNotificationSource(const NotificationSyncSource* source)
  {
    notificationSyncSource_ = source;
  }

  void setIdentitySource(const IdentitySyncSource* source)
  {
    identitySyncSource_ = source;
  }

  drogon::Task<Json::Value> sync(const SynchronizedDto& body,
                                 const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncAuditLog(const SynchronizedLogDto& body,
                                         const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncUserAuditLog(const SynchronizedLogDto& body,
                                             const JwtContext& ctx) const;

private:
  const CameraSyncSource* cameraSyncSource_{nullptr};
  const ProductivitySyncSource* productivitySyncSource_{nullptr};
  const NotificationSyncSource* notificationSyncSource_{nullptr};
  const IdentitySyncSource* identitySyncSource_{nullptr};
  UserActionLogRepository userActionLogRepository_;
  AuditLogRepository auditLogRepository_;
  UserAuditLogRepository userAuditLogRepository_;

  const Syncable& repoFor(TableName table) const;
  SyncFilter applyRange(const SyncFilter& base,
                        const std::optional<SynchronizedRangeDto>& range) const;
  drogon::Task<Json::Value>
  syncWithRepo(const SyncWithRepoInput& input, const SyncFilter& base) const;
  [[nodiscard]] drogon::Task<Json::Value>
  pullTable(const TablePullInput& input) const;
  drogon::Task<Json::Value>
  syncUserNotification(const SynchronizedBodyDto& dto,
                       const JwtContext& ctx) const;
  std::vector<TableName> auditTablesForRole(UserRole role) const;
};
