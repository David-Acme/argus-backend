#include "resource-change-outbox.hxx"

#include <feature/guard/controllers/guard-errors.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-backoff.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/stream-retention.hxx>
#include <sync/sync-change.hxx>
#include <text/json-util.hxx>
#include <utility>

namespace
{
outbox::OutboxConfig outboxConfig(const std::string& subject)
{
  return {.label = "Guard resource changes",
          .client = [] { return DbService::client(); },
          .defaultSubject = subject,
          .legacyIdPrefix = std::string(ResourceChangeOutbox::kEventIdPrefix),
          .refusal = GuardErrors::ChangeNotRecorded,
          .ensureStreams =
              [](NatsBus& bus) {
                return bus.ensureStream(
                    {.name = nats_subject::kGuardStream,
                     .subjects = {nats_subject::kGuardSubjectFilter},
                     .maxAgeNs = stream_retention::kRetentionNs,
                     .duplicatesNs = stream_retention::kDuplicatesNs});
              },
          .timing = {.retryMs = outbox::kRetryMs,
                     .maxRetryMs = outbox::kMaxRetryMs,
                     .progressMs = outbox::kProgressMs,
                     .batch = outbox::kBatch},
          .retention = {.keepSentMs = stream_retention::kRetentionMs,
                        .purgeEveryMs = stream_retention::kSettledPurgeIntervalMs,
                        .purgeRetryMs = stream_retention::kSettledPurgeRetryMs}};
}

Json::Value resourcePayload(const ResourceChangeInput& input)
{
  Json::Value payload(Json::objectValue);
  payload[sync_change::kKindField] = sync_change::kKindResource;
  payload[sync_change::kDomainField] =
      resourceDomainToString(ResourceDomain::Guard);
  payload[sync_change::kScopeField] = resourceScopeToString(input.scope);
  if (!input.ids.empty()) {
    Json::Value ids(Json::arrayValue);
    for (const auto id : input.ids)
      ids.append(static_cast<Json::Int64>(id));
    payload[sync_change::kIdsField] = std::move(ids);
  }
  return payload;
}
}

ResourceChangeOutbox::ResourceChangeOutbox(const std::shared_ptr<NatsBus>& bus,
                                           const Config& config)
    : subject_(config.subject.empty()
                   ? std::string(nats_subject::kGuardChange)
                   : config.subject),
      outbox_(bus, outboxConfig(subject_))
{
}

drogon::Task<void>
ResourceChangeOutbox::record(const ResourceChangeInput& input) const
{
  co_await outbox_.append({.idPrefix = kEventIdPrefix,
                           .subject = subject_,
                           .payload = json_util::toString(resourcePayload(input)),
                           .client = input.client});
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
