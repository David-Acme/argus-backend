#include "nats-camera-change-sink.hxx"

#include <camera/camera-errors.hxx>
#include <chrono>
#include <errors/response-exception.hxx>
#include <nats/nats-subject.hxx>
#include <outbox/outbox-key.hxx>
#include <shared/services/event-stream/event-stream.hxx>
#include <sqlite/db-service.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string changeSubject(const std::string& configured)
{
  return configured.empty() ? std::string(nats_subject::kCameraChange)
                            : configured;
}

outbox::OutboxConfig outboxConfig(const std::shared_ptr<NatsBus>& bus,
                                  const NatsCameraChangeSink::Config& config)
{
  const camera_event_stream::EnsureInput stream{
      .streamName = config.streamName,
      .changeSubject = config.publishSubject,
      .objectSubject = {}};
  return {.label = "Camera change outbox",
          .client = [] { return DbService::cameraClient(); },
          .defaultSubject = changeSubject(config.publishSubject),
          .legacyIdPrefix = std::string(NatsCameraChangeSink::kEventIdPrefix),
          .refusal = CameraErrors::ChangeNotRecorded,
          .ensureStreams =
              [bus, stream](NatsBus&) {
                return camera_event_stream::ensure(bus, stream);
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

NatsCameraChangeSink::NatsCameraChangeSink(
    const std::shared_ptr<NatsBus>& bus, const Config& config)
    : subject_(changeSubject(config.publishSubject)),
      outbox_(bus, outboxConfig(bus, config))
{
}

outbox::OutboxRepository NatsCameraChangeSink::repository()
{
  return outbox::OutboxRepository([] { return DbService::cameraClient(); });
}

drogon::Task<void>
NatsCameraChangeSink::emitModule(const ModuleEmitInput& input) const
{
  const Json::Value& recordId = input.body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Camera change outbox: an emit of "
              << tableNameToString(input.table)
              << " carried no record id; the write is refused";
    throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  co_await record({.table = input.table,
                   .recordId = recordId.asInt64(),
                   .payload = json_util::toString(input.body.toJson()),
                   .client = input.client});
}

drogon::Task<void>
NatsCameraChangeSink::publishAudit(const ModuleAuditInput& input) const
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

drogon::Task<void> NatsCameraChangeSink::record(RecordInput input) const
{
  std::string eventId =
      outbox::transitionId({.prefix = kEventIdPrefix,
                            .table = tableNameToString(input.table),
                            .recordId = input.recordId,
                            .discriminator = input.payload});
  static_cast<void>(co_await outbox_.record({.eventId = std::move(eventId),
                                             .subject = subject_,
                                             .payload = std::move(input.payload),
                                             .client = input.client}));
}

void NatsCameraChangeSink::reconcile()
{
  outbox_.reconcile();
}

void NatsCameraChangeSink::requestStop()
{
  outbox_.requestStop();
}

bool NatsCameraChangeSink::drained() const
{
  return outbox_.drained();
}
