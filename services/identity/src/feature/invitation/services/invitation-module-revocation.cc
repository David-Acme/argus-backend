#include "invitation-module-revocation.hxx"

#include <algorithm>
#include <auth/module-gate.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>

#include <utility>

namespace
{
constexpr int64_t kSystemActor = 0;
}

drogon::Task<std::vector<UserInvitationSchema>>
InvitationModuleRevocation::revoke(const InvitationModuleRevocationInput& input) const
{
  std::vector<UserInvitationSchema> revoked;
  if (input.roles.empty())
    co_return revoked;
  auto transaction = co_await db_transaction::begin(DbService::identityClient());
  try {
    const auto pending = co_await repository_.findPending(
        {.roles = input.roles, .now = static_cast<int64_t>(std::time(nullptr)), .client = transaction.get()});
    for (const auto& before : pending) {
      const bool done = co_await repository_.revokeForModule({.invitationId = before.id,
                                                              .reason = InvitationRevocationReason::ModuleDisabled,
                                                              .moduleId = input.moduleId,
                                                              .client = transaction.get()});
      if (!done)
        continue;
      const auto after = co_await repository_.findById(before.id, transaction.get());
      if (!after)
        continue;
      if (const auto* sink = identity_change::getSink()) {
        co_await sink->publishModuleAudit({.recordId = after->id,
                                           .tableName = TableName::UserInvitation,
                                           .before = before.toJson(),
                                           .after = after->toJson(),
                                           .actorId = std::nullopt,
                                           .client = transaction.get()});
        co_await sink->publishAction({.event = {.userId = kSystemActor,
                                                .recordId = after->id,
                                                .tableName = TableName::UserInvitation,
                                                .action = UserAction::Update,
                                                .oldData = before.toJson(),
                                                .newData = after->toJson(),
                                                .ipAddress = ""},
                                      .client = transaction.get()});
      }
      revoked.push_back(*after);
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return revoked;
}

drogon::Task<std::size_t> InvitationModuleRevocation::revokeModule(std::string moduleId) const
{
  const auto modules = moduleGate().current();
  const auto found = std::ranges::find(modules->modules(), moduleId, &ModuleFlag::id);
  if (found == modules->modules().end())
    co_return 0;
  const auto revoked = co_await revoke({.moduleId = moduleId, .roles = found->roles});
  co_return revoked.size();
}

drogon::Task<std::size_t> InvitationModuleRevocation::revokeDisabledModules() const
{
  std::size_t total = 0;
  const auto modules = moduleGate().current();
  for (const auto& module : modules->modules()) {
    if (module.enabled)
      continue;
    const auto revoked = co_await revoke({.moduleId = module.id, .roles = module.roles});
    total += revoked.size();
  }
  co_return total;
}
