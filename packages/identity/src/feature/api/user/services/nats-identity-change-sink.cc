#include "nats-identity-change-sink.hxx"

#include <chrono>
#include <sync/user-audit-event.hxx>
#include <sync/sync-change.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <vector>

NatsIdentityChangeSink::NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsIdentityChangeSink::publishCatalog(
    const IdentityCatalogInput& input) const
{
  Json::Value event(Json::objectValue);
  event[sync_change::kKindField] = sync_change::kKindIdentity;
  event["table"] = tableNameToString(input.table);
  event["id"] = static_cast<Json::Int64>(input.id);
  event["deleted"] = input.deleted;
  event["row"] = input.row;

  if (!bus_->publish(nats_subject::kIdentityChange, json_util::toString(event)))
    LOG_WARN << "Identity funnel: catalog publish failed for "
             << tableNameToString(input.table) << " " << input.id;
}

void NatsIdentityChangeSink::emitModule(TableName table,
                                        const SocketEmitDto& body) const
{
  SocketEmitDto frame = body;
  frame.option = table;

  if (!bus_->publish(nats_subject::kIdentityChange,
                     json_util::toString(sync_change::emitPayload(frame))))
    LOG_WARN << "Identity funnel: emit failed for "
             << tableNameToString(table);
}

drogon::Task<void> NatsIdentityChangeSink::publishModuleAudit(
    const ModuleAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  ModuleAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.createUserId = input.actorId;
  event.eventTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (!bus_->publish(nats_subject::kIdentityChange,
                     json_util::toString(event.toJson())))
    LOG_WARN << "Identity audit funnel: publish failed for record "
             << input.recordId;
  co_return;
}

drogon::Task<void> NatsIdentityChangeSink::publishUsersAudit(
    const UserAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  std::vector<int64_t> recipients;
  std::unordered_set<int64_t> seen;
  for (const auto userId : input.userIds) {
    if (userId <= 0 || !seen.insert(userId).second)
      continue;
    recipients.push_back(userId);
  }
  if (recipients.empty())
    co_return;

  UserAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.users = std::move(recipients);
  event.eventTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (!bus_->publish(nats_subject::kIdentityChange,
                     json_util::toString(event.toJson())))
    LOG_WARN << "Identity audit funnel: publish failed for record "
             << input.recordId;
  co_return;
}

drogon::Task<void>
NatsIdentityChangeSink::publishAction(const UserActionEvent& event) const
{
  if (!bus_->publish(nats_subject::kIdentityUserAction,
                     json_util::toString(event.toJson())))
    LOG_WARN << "Identity action funnel: publish failed for record "
             << event.recordId;
  co_return;
}
