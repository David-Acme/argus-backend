#include "nats-productivity-change-sink.hxx"

#include <chrono>
#include <shared/contracts/user-audit-event.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <unordered_set>

NatsProductivityChangeSink::NatsProductivityChangeSink(
    std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsProductivityChangeSink::emitUser(int64_t userId,
                                          const SocketEmitDto& body) const
{
  bus_->publish(nats_subject::kProductivityChange,
                json_util::toString(sync_change::userEmitPayload(body,
                                                                 {userId})));
}

void NatsProductivityChangeSink::emitUsers(
    const std::vector<int64_t>& userIds, const SocketEmitDto& body) const
{
  bus_->publish(nats_subject::kProductivityChange,
                json_util::toString(sync_change::userEmitPayload(body,
                                                                 userIds)));
}

drogon::Task<void> NatsProductivityChangeSink::publishAudit(
    const UserAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  // The same recipient set the legacy SyncAuditService::publishUsers kept:
  // non-positive ids out, duplicates collapsed.
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

  if (!bus_->publish(nats_subject::kProductivityChange,
                     json_util::toString(event.toJson())))
    LOG_WARN << "Productivity audit funnel: publish failed for record "
             << input.recordId;
  co_return;
}