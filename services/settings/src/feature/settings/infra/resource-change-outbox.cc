#include "resource-change-outbox.hxx"

#include <drogon/drogon.h>
#include <feature/settings/settings-gateway-errors.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-backoff.hxx>
#include <string>
#include <sync/resource-change.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
outbox::OutboxConfig outboxConfig(outbox::ClientAccessor client)
{
  return {.label = "Settings change outbox",
          .client = std::move(client),
          .defaultSubject = std::string(nats_subject::kSettingsChange),
          .legacyIdPrefix = std::string(SettingsChangeOutbox::kEventIdPrefix),
          .refusal = SettingsGatewayErrors::ChangeNotRecorded,
          .ensureStreams =
              [](NatsBus& bus) {
                return bus.ensureStream(
                    {.name = nats_subject::kSettingsChangeStream,
                     .subjects = {nats_subject::kSettingsChange},
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
}

SettingsChangeOutbox::SettingsChangeOutbox(const std::shared_ptr<NatsBus>& bus,
                                           outbox::ClientAccessor client)
    : client_(std::move(client)), outbox_(bus, outboxConfig(client_))
{
}

drogon::Task<void> SettingsChangeOutbox::record() const
{
  co_await outbox_.append(
      {.idPrefix = kEventIdPrefix,
       .subject = std::string(nats_subject::kSettingsChange),
       .payload = json_util::toString(resource_change::payload(
           ResourceDomain::Settings, ResourceScope::Values, {})),
       .client = nullptr});
}

void SettingsChangeOutbox::recordDetached() const
{
  drogon::async_run([this]() -> drogon::Task<> {
    try {
      co_await record();
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Settings: a change was applied but its notice was not "
                   "recorded: "
                << error.what();
    }
  });
}

bool SettingsChangeOutbox::migrate() const
{
  return outbox::OutboxRepository(client_).migrateSchema();
}

void SettingsChangeOutbox::reconcile()
{
  outbox_.reconcile();
}

void SettingsChangeOutbox::requestStop()
{
  outbox_.requestStop();
}

bool SettingsChangeOutbox::drained() const
{
  return outbox_.drained();
}
