#pragma once

#include <memory>
#include <sync/identity-change-sink.hxx>

class NatsBus;

// NATS substrate of the identity-domain change events; the sync fan-out
// ignores the catalog payloads and dispatches the rest.
class NatsIdentityChangeSink : public IdentityChangeSink
{
public:
  explicit NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus);

  void publishCatalog(const IdentityCatalogInput& input) const override;

  void emitModule(TableName table, const SocketEmitDto& body) const override;

  drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override;

  drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const override;

  drogon::Task<void> publishAction(const UserActionEvent& event) const override;

private:
  std::shared_ptr<NatsBus> bus_;
};
