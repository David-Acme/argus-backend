#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/table-name.hxx>
#include <sync/socket-emit-dto.hxx>
#include <vector>

struct UserAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Notification};
  Json::Value before;
  Json::Value after;
  std::vector<int64_t> userIds;
};

class AuditSink
{
public:
  virtual ~AuditSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  publishAudit(const UserAuditInput& input) const = 0;
};

class UserChangeSink : public AuditSink
{
public:
  [[nodiscard]] virtual drogon::Task<void>
  emitUser(int64_t userId, const SocketEmitDto& body) const = 0;

  [[nodiscard]] virtual drogon::Task<void>
  emitUsers(const std::vector<int64_t>& userIds,
            const SocketEmitDto& body) const = 0;
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

inline const AuditSink*& notificationSink()
{
  static const AuditSink* instance = nullptr;
  return instance;
}

inline void setNotificationSink(const AuditSink* value)
{
  notificationSink() = value;
}

inline const AuditSink* getNotificationSink()
{
  return notificationSink();
}
}
