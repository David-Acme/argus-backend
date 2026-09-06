#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <vector>

// Input of the user-scoped audit publication: the before/after snapshots of
// one persisted row plus the recipients that would receive the user_audit_log
// row. The sink computes the same flat diff and recipient set the legacy
// SyncAuditService::publishUsers produces.
struct UserAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Notification};
  Json::Value before;
  Json::Value after;
  std::vector<int64_t> userIds;
};

// Substrate of the productivity and notification domain change events. The
// legacy binds the local SocketService rooms plus the SyncAuditService
// publication; argus-productivity and argus-notification funnel the same
// payloads over their own NATS subjects (Rulings AQ/AR). Installed once at
// boot, before the service serves.
class UserChangeSink
{
public:
  virtual ~UserChangeSink() = default;

  virtual void emitUser(int64_t userId, const SocketEmitDto& body) const = 0;

  virtual void emitUsers(const std::vector<int64_t>& userIds,
                         const SocketEmitDto& body) const = 0;

  virtual drogon::Task<void>
  publishAudit(const UserAuditInput& input) const = 0;
};

namespace user_change
{
inline const UserChangeSink*& productivitySink()
{
  static const UserChangeSink* instance = nullptr;
  return instance;
}

inline void setProductivitySink(const UserChangeSink* value)
{
  productivitySink() = value;
}

inline const UserChangeSink* getProductivitySink()
{
  return productivitySink();
}

inline const UserChangeSink*& notificationSink()
{
  static const UserChangeSink* instance = nullptr;
  return instance;
}

inline void setNotificationSink(const UserChangeSink* value)
{
  notificationSink() = value;
}

inline const UserChangeSink* getNotificationSink()
{
  return notificationSink();
}
} // namespace user_change
