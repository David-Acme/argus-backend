#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/socket/sync/dtos/synchronized-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <shared/contracts/syncable.hxx>
#include <shared/contracts/sync-filter.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/repositories/camera-stream/camera-stream-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/repositories/reminder-detail/reminder-detail-repository.hxx>
#include <shared/repositories/reminder/reminder-repository.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
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

  drogon::Task<Json::Value> sync(const SynchronizedDto& body,
                                 const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncAuditLog(const SynchronizedLogDto& body,
                                         const JwtContext& ctx) const;
  drogon::Task<Json::Value> syncUserAuditLog(const SynchronizedLogDto& body,
                                             const JwtContext& ctx) const;

private:
  UserRepository userRepository_;
  CameraRepository cameraRepository_;
  CameraStreamRepository cameraStreamRepository_;
  ZoneRepository zoneRepository_;
  ReminderRepository reminderRepository_;
  ReminderDetailRepository reminderDetailRepository_;
  NotificationRepository notificationRepository_;
  AuditLogRepository auditLogRepository_;
  UserAuditLogRepository userAuditLogRepository_;

  const Syncable& repoFor(TableName table) const;
  SyncFilter applyRange(const SyncFilter& base,
                        const std::optional<SynchronizedRangeDto>& range) const;
  drogon::Task<Json::Value>
  syncWithRepo(const SyncWithRepoInput& input, const SyncFilter& base) const;
  drogon::Task<Json::Value>
  syncUserNotification(const SynchronizedBodyDto& dto, int64_t userId) const;
  std::vector<TableName> auditTablesForRole(UserRole role) const;
};
