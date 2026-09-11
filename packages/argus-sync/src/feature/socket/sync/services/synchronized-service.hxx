#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/dtos/synchronized-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <shared/contracts/syncable.hxx>
#include <shared/contracts/camera-sync-source.hxx>
#include <shared/contracts/notification-sync-source.hxx>
#include <shared/contracts/productivity-sync-source.hxx>
#include <shared/contracts/sync-filter.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/event/event-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <vector>

struct SyncWithRepoInput
{
  const Syncable& repo;
  const SynchronizedBodyDto& dto;
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

  drogon::Task<Json::Value> sync(const SynchronizedDto& body,
                                 const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncAuditLog(const SynchronizedLogDto& body,
                                         const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncUserAuditLog(const SynchronizedLogDto& body,
                                             const JwtContext& ctx) const;

private:
  UserRepository userRepository_;
  UserInvitationRepository userInvitationRepository_;
  const CameraSyncSource* cameraSyncSource_{nullptr};
  const ProductivitySyncSource* productivitySyncSource_{nullptr};
  const NotificationSyncSource* notificationSyncSource_{nullptr};
  EventRepository eventRepository_;
  PersonRepository personRepository_;
  AuditLogRepository auditLogRepository_;
  UserAuditLogRepository userAuditLogRepository_;

  const Syncable& repoFor(TableName table) const;
  SyncFilter applyRange(const SyncFilter& base,
                        const std::optional<SynchronizedRangeDto>& range) const;
  drogon::Task<Json::Value>
  syncWithRepo(const SyncWithRepoInput& input, const SyncFilter& base) const;
  drogon::Task<Json::Value>
  syncUserNotification(const SynchronizedBodyDto& dto,
                       const JwtContext& ctx) const;
  std::vector<TableName> auditTablesForRole(UserRole role) const;
};
