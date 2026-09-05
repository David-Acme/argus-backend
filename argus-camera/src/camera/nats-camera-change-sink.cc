#include "nats-camera-change-sink.hxx"

#include <chrono>
#include <shared/contracts/camera-audit-event.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

NatsCameraChangeSink::NatsCameraChangeSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsCameraChangeSink::emitModule(TableName table,
                                      const SocketEmitDto& body) const
{
  (void)table;
  bus_->publish(nats_subject::kCameraChange, json_util::toString(body.toJson()));
}

drogon::Task<void> NatsCameraChangeSink::publishAudit(
    const CameraAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  CameraAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.createUserId = input.actorId;
  event.eventTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (!bus_->publish(nats_subject::kCameraChange,
                     json_util::toString(event.toJson())))
    LOG_WARN << "Camera audit funnel: publish failed for record "
             << input.recordId;
  co_return;
}