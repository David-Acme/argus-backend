#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-change-sink.hxx>

struct IdentityCatalogInput
{
  TableName table{TableName::User};
  int64_t id{0};
  bool deleted{false};
  Json::Value row;
  drogon::orm::DbClient* client{nullptr};
};

struct ActionPublishInput
{
  UserActionEvent event;
  drogon::orm::DbClient* client{nullptr};
};

class IdentityChangeSink
{
public:
  virtual ~IdentityChangeSink() = default;

  virtual drogon::Task<void> publishCatalog(
      const IdentityCatalogInput& input) const = 0;

  virtual drogon::Task<void> emitModule(const ModuleEmitInput& input) const = 0;

  virtual drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const = 0;

  virtual drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const = 0;

  virtual drogon::Task<void>
  publishAction(const ActionPublishInput& input) const = 0;
};

namespace identity_change
{
inline const IdentityChangeSink*& sink()
{
  static const IdentityChangeSink* instance = nullptr;
  return instance;
}

inline void setSink(const IdentityChangeSink* value)
{
  sink() = value;
}

inline const IdentityChangeSink* getSink()
{
  return sink();
}
}
