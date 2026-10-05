#include "nats-productivity-change-sink.hxx"

#include <chrono>
#include <errors/response-exception.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-key.hxx>
#include <productivity/productivity-errors.hxx>
#include <sqlite/db-service.hxx>
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

outbox::OutboxConfig
outboxConfig(const NatsProductivityChangeSink::Config& config)
{
  NatsBus::StreamInput stream{
      .name = orDefault(config.streamName,
                        nats_subject::kProductivityChangeStream),
      .subjects = {orDefault(config.publishSubject,
                             nats_subject::kProductivityChange)},
      .maxAgeNs = stream_retention::kRetentionNs,
      .duplicatesNs = stream_retention::kDuplicatesNs};
  return {.label = "Productivity change outbox",
          .client = [] { return DbService::productivityClient(); },
          .defaultSubject = stream.subjects.front(),
          .legacyIdPrefix =
              std::string(NatsProductivityChangeSink::kEventIdPrefix),
          .refusal = ProductivityErrors::ChangeNotRecorded,
          .ensureStreams =
              [stream = std::move(stream)](NatsBus& bus) {
                if (!bus.ensureStream(stream))
                  return false;
                LOG_INFO << "Productivity change outbox: stream "
                         << stream.name << " ready";
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
}

NatsProductivityChangeSink::NatsProductivityChangeSink(
    std::shared_ptr<NatsBus> bus, const Config& config)
    : subject_(orDefault(config.publishSubject,
                         nats_subject::kProductivityChange)),
      outbox_(std::move(bus), outboxConfig(config))
{
}

outbox::OutboxRepository NatsProductivityChangeSink::repository()
{
  return outbox::OutboxRepository(
      [] { return DbService::productivityClient(); });
}

drogon::Task<void>
NatsProductivityChangeSink::emitUsers(const UserEmitInput& input) const
{
  const Json::Value& recordId = input.body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Productivity change outbox: an emit of "
              << tableNameToString(input.body.option)
              << " carried no record id; the write is refused";
    throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  std::string payload = json_util::toString(
      sync_change::userEmitPayload(input.body, input.userIds));
  std::string transition =
      payload + '|' + std::to_string(nowMs()) + '|' +
      std::to_string(transitions_.fetch_add(1, std::memory_order_relaxed));
  co_await record({.table = input.body.option,
                   .recordId = recordId.asInt64(),
                   .discriminator = std::move(transition),
                   .payload = std::move(payload),
                   .client = input.client});
}

drogon::Task<void>
NatsProductivityChangeSink::publishAudit(const UserAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  std::vector<int64_t> recipients;
  std::unordered_set<int64_t> seen;
  for (const auto userId : input.userIds) {
    if (userId > 0 && seen.insert(userId).second)
      recipients.push_back(userId);
  }
  if (recipients.empty())
    co_return;

  UserAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.users = std::move(recipients);
  event.eventTimestamp = nowMs();
  std::string payload = json_util::toString(event.toJson());
  co_await record({.table = input.tableName,
                   .recordId = input.recordId,
                   .discriminator = payload,
                   .payload = std::move(payload),
                   .client = input.client});
}

drogon::Task<void> NatsProductivityChangeSink::record(RecordInput input) const
{
  std::string eventId =
      outbox::transitionId({.prefix = kEventIdPrefix,
                            .table = tableNameToString(input.table),
                            .recordId = input.recordId,
                            .discriminator = input.discriminator});
  static_cast<void>(co_await outbox_.record({.eventId = std::move(eventId),
                                             .subject = subject_,
                                             .payload = std::move(input.payload),
                                             .client = input.client}));
}

void NatsProductivityChangeSink::reconcile()
{
  outbox_.reconcile();
}

void NatsProductivityChangeSink::requestStop()
{
  outbox_.requestStop();
}

bool NatsProductivityChangeSink::drained() const
{
  return outbox_.drained();
}
