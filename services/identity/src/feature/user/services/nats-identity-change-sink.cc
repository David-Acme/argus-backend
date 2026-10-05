#include "nats-identity-change-sink.hxx"

#include <chrono>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-key.hxx>
#include <sqlite/db-service.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/stream-retention.hxx>
#include <sync/sync-change.hxx>
#include <sync/user-audit-event.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string orDefault(const std::string& configured, std::string_view fallback)
{
  return configured.empty() ? std::string(fallback) : configured;
}

outbox::OutboxConfig outboxConfig(const NatsIdentityChangeSink::Config& config)
{
  NatsBus::StreamInput stream{
      .name = orDefault(config.streamName, nats_subject::kIdentityChangeStream),
      .subjects = {orDefault(config.changeSubject,
                             nats_subject::kIdentityChange),
                   orDefault(config.actionSubject,
                             nats_subject::kIdentityUserAction)},
      .maxAgeNs = stream_retention::kRetentionNs,
      .duplicatesNs = stream_retention::kDuplicatesNs};
  return {.label = "Identity change outbox",
          .client = [] { return DbService::identityClient(); },
          .defaultSubject = stream.subjects.front(),
          .legacyIdPrefix = std::string(NatsIdentityChangeSink::kActionIdPrefix),
          .refusal = IdentityErrors::ChangeNotRecorded,
          .ensureStreams =
              [stream = std::move(stream)](NatsBus& bus) {
                if (!bus.ensureStream(stream))
                  return false;
                LOG_INFO << "Identity change outbox: stream " << stream.name
                         << " ready";
                return true;
              },
          .timing = {.retryMs = config.retryMs,
                     .maxRetryMs = outbox::kMaxRetryMs,
                     .progressMs = outbox::kProgressMs,
                     .batch = outbox::kBatch},
          .retention = {.keepSentMs = stream_retention::kRetentionMs,
                        .purgeEveryMs = stream_retention::kSettledPurgeIntervalMs,
                        .purgeRetryMs = stream_retention::kSettledPurgeRetryMs}};
}

std::vector<int64_t> distinctRecipients(const std::vector<int64_t>& userIds)
{
  std::vector<int64_t> recipients;
  std::unordered_set<int64_t> seen;
  for (const auto userId : userIds) {
    if (userId > 0 && seen.insert(userId).second)
      recipients.push_back(userId);
  }
  return recipients;
}
}

NatsIdentityChangeSink::NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus,
                                               const Config& config)
    : changeSubject_(
          orDefault(config.changeSubject, nats_subject::kIdentityChange)),
      actionSubject_(
          orDefault(config.actionSubject, nats_subject::kIdentityUserAction)),
      outbox_(std::move(bus), outboxConfig(config))
{
}

outbox::OutboxRepository NatsIdentityChangeSink::repository()
{
  return outbox::OutboxRepository([] { return DbService::identityClient(); });
}

drogon::Task<void>
NatsIdentityChangeSink::publishCatalog(const IdentityCatalogInput& input) const
{
  Json::Value event(Json::objectValue);
  event[sync_change::kKindField] = sync_change::kKindIdentity;
  event[sync_change::kTableField] = tableNameToString(input.table);
  event[sync_change::kRecordIdField] = static_cast<Json::Int64>(input.id);
  event[sync_change::kDeletedField] = input.deleted;
  event[sync_change::kRowField] = input.row;
  co_await record({.table = input.table,
                   .recordId = input.id,
                   .payload = json_util::toString(event),
                   .client = input.client});
}

drogon::Task<void>
NatsIdentityChangeSink::emitModule(const ModuleEmitInput& input) const
{
  const Json::Value& recordId = input.body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Identity change outbox: an emit of "
              << tableNameToString(input.table)
              << " carried no record id; the write is refused";
    throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  SocketEmitDto frame = input.body;
  frame.option = input.table;
  co_await record(
      {.table = input.table,
       .recordId = recordId.asInt64(),
       .payload = json_util::toString(sync_change::emitPayload(frame)),
       .client = input.client});
}

drogon::Task<void>
NatsIdentityChangeSink::publishModuleAudit(const ModuleAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  ModuleAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.createUserId = input.actorId;
  event.eventTimestamp = nowMs();
  co_await record({.table = input.tableName,
                   .recordId = input.recordId,
                   .payload = json_util::toString(event.toJson()),
                   .client = input.client});
}

drogon::Task<void>
NatsIdentityChangeSink::publishUsersAudit(const UserAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  auto recipients = distinctRecipients(input.userIds);
  if (recipients.empty())
    co_return;

  UserAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.users = std::move(recipients);
  event.eventTimestamp = nowMs();
  co_await record({.table = input.tableName,
                   .recordId = input.recordId,
                   .payload = json_util::toString(event.toJson()),
                   .client = input.client});
}

drogon::Task<void>
NatsIdentityChangeSink::publishAction(const ActionPublishInput& input) const
{
  co_await outbox_.append({.idPrefix = kActionIdPrefix,
                           .subject = actionSubject_,
                           .payload = json_util::toString(input.event.toJson()),
                           .client = input.client});
}

drogon::Task<void> NatsIdentityChangeSink::record(RecordInput input) const
{
  std::string eventId =
      outbox::transitionId({.prefix = kEventIdPrefix,
                            .table = tableNameToString(input.table),
                            .recordId = input.recordId,
                            .discriminator = input.payload});
  static_cast<void>(co_await outbox_.record({.eventId = std::move(eventId),
                                             .subject = changeSubject_,
                                             .payload = std::move(input.payload),
                                             .client = input.client}));
}

void NatsIdentityChangeSink::reconcile()
{
  outbox_.reconcile();
}

void NatsIdentityChangeSink::requestStop()
{
  outbox_.requestStop();
}

bool NatsIdentityChangeSink::drained() const
{
  return outbox_.drained();
}
