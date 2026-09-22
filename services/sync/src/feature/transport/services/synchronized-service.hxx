#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/transport/dtos/synchronized-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <feature/transport/infra/camera-sync-source.hxx>
#include <feature/transport/infra/notification-sync-source.hxx>
#include <feature/transport/infra/productivity-sync-source.hxx>
#include <sync/sync-filter.hxx>
#include <sync/syncable.hxx>
#include <sync/table-name.hxx>
#include <sync/socket-emit-dto.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <feature/transport/repositories/event/event-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <auth/user-role.hxx>
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
  UserActionLogRepository userActionLogRepository_;
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
