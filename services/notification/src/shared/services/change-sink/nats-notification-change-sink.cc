#include "nats-notification-change-sink.hxx"

#include <chrono>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <notification/notification-errors.hxx>
#include <outbox/outbox-key.hxx>
#include <sqlite/db-service.hxx>
#include <sync/stream-retention.hxx>
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
outboxConfig(const NatsNotificationChangeSink::Config& config)
{
  NatsBus::StreamInput stream{
      .name = orDefault(config.streamName,
                        nats_subject::kNotificationChangeStream),
      .subjects = {orDefault(config.publishSubject,
                             nats_subject::kNotificationChange)},
      .maxAgeNs = stream_retention::kRetentionNs,
      .duplicatesNs = stream_retention::kDuplicatesNs};
  return {.label = "Notification change outbox",
          .client = [] { return DbService::client(); },
          .defaultSubject = stream.subjects.front(),
          .legacyIdPrefix =
              std::string(NatsNotificationChangeSink::kEventIdPrefix),
          .refusal = NotificationErrors::ChangeNotRecorded,
          .ensureStreams =
              [stream = std::move(stream)](NatsBus& bus) {
                if (!bus.ensureStream(stream))
                  return false;
                LOG_INFO << "Notification change outbox: stream "
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

NatsNotificationChangeSink::NatsNotificationChangeSink(
    std::shared_ptr<NatsBus> bus, const Config& config)
    : subject_(orDefault(config.publishSubject,
                         nats_subject::kNotificationChange)),
      outbox_(std::move(bus), outboxConfig(config))
{
}

outbox::OutboxRepository NatsNotificationChangeSink::repository()
{
  return outbox::OutboxRepository([] { return DbService::client(); });
}

drogon::Task<void> NatsNotificationChangeSink::publishAudit(
    const UserAuditInput& input) const
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
  std::string eventId =
      outbox::transitionId({.prefix = kEventIdPrefix,
                            .table = tableNameToString(input.tableName),
                            .recordId = input.recordId,
                            .discriminator = payload});
  static_cast<void>(co_await outbox_.record({.eventId = std::move(eventId),
                                             .subject = subject_,
                                             .payload = std::move(payload),
                                             .client = input.client}));
}

void NatsNotificationChangeSink::reconcile()
{
  outbox_.reconcile();
}

void NatsNotificationChangeSink::requestStop()
{
  outbox_.requestStop();
}

bool NatsNotificationChangeSink::drained() const
{
  return outbox_.drained();
}
