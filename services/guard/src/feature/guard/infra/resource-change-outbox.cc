#include "resource-change-outbox.hxx"

#include <feature/guard/controllers/guard-errors.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-backoff.hxx>
#include <outbox/outbox-key.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/stream-retention.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <utility>

namespace
{
bool ensureGuardStreams(NatsBus& bus)
{
  return bus.ensureStream(
             {.name = nats_subject::kGuardStream,
              .subjects = {nats_subject::kGuardSubjectFilter},
              .maxAgeNs = stream_retention::kRetentionNs,
              .duplicatesNs = stream_retention::kDuplicatesNs}) &&
         bus.ensureStream(
             {.name = nats_subject::kGuardActionStream,
              .subjects = {nats_subject::kGuardUserAction},
              .maxAgeNs = stream_retention::kRetentionNs,
              .duplicatesNs = stream_retention::kDuplicatesNs});
}

outbox::OutboxConfig outboxConfig()
{
  return {.label = "Guard resource changes",
          .client = [] { return DbService::client(); },
          .defaultSubject = std::string(nats_subject::kGuardChange),
          .legacyIdPrefix = std::string(ResourceChangeOutbox::kEventIdPrefix),
          .refusal = GuardErrors::ChangeNotRecorded,
          .ensureStreams = ensureGuardStreams,
          .timing = {.retryMs = outbox::kRetryMs,
                     .maxRetryMs = outbox::kMaxRetryMs,
                     .progressMs = outbox::kProgressMs,
                     .batch = outbox::kBatch},
          .retention = {.keepSentMs = stream_retention::kRetentionMs,
                        .purgeEveryMs = stream_retention::kSettledPurgeIntervalMs,
                        .purgeRetryMs = stream_retention::kSettledPurgeRetryMs}};
}
}

ResourceChangeRow resourceChangeRow(const ResourceChangeInput& input)
{
  return {.subject = std::string(nats_subject::kGuardChange),
          .payload = json_util::toString(resource_change::payload(
              input.domain, input.scope, input.ids, input.user))};
}

ResourceChangeOutbox::ResourceChangeOutbox(const std::shared_ptr<NatsBus>& bus)
    : outbox_(bus, outboxConfig())
{
}

drogon::Task<void>
ResourceChangeOutbox::record(const ResourceChangeInput& input) const
{
  const ResourceChangeRow row = resourceChangeRow(input);
  co_await outbox_.append({.idPrefix = kEventIdPrefix,
                           .subject = row.subject,
                           .payload = row.payload,
                           .client = input.client});
}

drogon::Task<void>
ResourceChangeOutbox::publishAction(const ActionPublishInput& input) const
{
  const UserActionEvent& event = input.event;
  const std::string payload = json_util::toString(event.toJson());
  const std::string eventId =
      outbox::transitionId({.prefix = kActionIdPrefix,
                            .table = tableNameToString(event.tableName),
                            .recordId = event.recordId,
                            .discriminator = payload});
  static_cast<void>(co_await outbox_.record(
      {.eventId = eventId,
       .subject = std::string(nats_subject::kGuardUserAction),
       .payload = payload,
       .client = input.client}));
}

drogon::Task<void>
ResourceChangeOutbox::recordEvent(const GuardEventInput& input) const
{
  static_cast<void>(co_await outbox_.record({.eventId = input.eventId,
                                             .subject = input.subject,
                                             .payload = input.payload,
                                             .client = input.client}));
}

outbox::OutboxRepository ResourceChangeOutbox::repository()
{
  return outbox::OutboxRepository([] { return DbService::client(); });
}

void ResourceChangeOutbox::reconcile()
{
  outbox_.reconcile();
}

void ResourceChangeOutbox::requestStop()
{
  outbox_.requestStop();
}

bool ResourceChangeOutbox::drained() const
{
  return outbox_.drained();
}
