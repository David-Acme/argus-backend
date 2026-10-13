#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/table-name.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/user-action-event.hxx>
#include <vector>

struct UserAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Notification};
  Json::Value before;
  Json::Value after;
  std::vector<int64_t> userIds;
  drogon::orm::DbClient* client{nullptr};
};

struct UserEmitInput
{
  std::vector<int64_t> userIds;
  SocketEmitDto body;
  drogon::orm::DbClient* client{nullptr};
};

struct ActionPublishInput
{
  UserActionEvent event;
  drogon::orm::DbClient* client{nullptr};
};

class AuditSink
{
public:
  virtual ~AuditSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  publishAudit(const UserAuditInput& input) const = 0;
};

class UserEmitSink : public AuditSink
{
public:
  [[nodiscard]] virtual drogon::Task<void>
  emitUsers(const UserEmitInput& input) const = 0;
};

class UserChangeSink : public UserEmitSink
{
public:
  [[nodiscard]] virtual drogon::Task<void>
  publishAction(const ActionPublishInput& input) const = 0;
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

inline const UserEmitSink*& notificationSink()
{
  static const UserEmitSink* instance = nullptr;
  return instance;
}

inline void setNotificationSink(const UserEmitSink* value)
{
  notificationSink() = value;
}

inline const UserEmitSink* getNotificationSink()
{
  return notificationSink();
}
}
