#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/module-audit-event.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-change-sink.hxx>

// Post-write snapshot of one user or person row; `deleted` marks a soft delete
// and `table` names the row's table the way the frozen feed spells it.
struct IdentityCatalogInput
{
  TableName table{TableName::User};
  int64_t id{0};
  bool deleted{false};
  Json::Value row;
};

// The identity domain's whole outbound wire: the catalog rows the memory
// replicas follow, the module and user audit rows, the action journal and the
// imperative emits. The host installs its NATS funnel at boot.
class IdentityChangeSink
{
public:
  virtual ~IdentityChangeSink() = default;

  virtual void publishCatalog(const IdentityCatalogInput& input) const = 0;

  virtual void emitModule(TableName table, const SocketEmitDto& body) const = 0;

  virtual drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const = 0;

  virtual drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const = 0;

  virtual drogon::Task<void>
  publishAction(const UserActionEvent& event) const = 0;
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
} // namespace identity_change
