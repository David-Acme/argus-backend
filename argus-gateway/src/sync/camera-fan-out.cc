#include "camera-fan-out.hxx"

#include <drogon/drogon.h>
#include <shared/contracts/camera-audit-event.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/services/audit-log/audit-log-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <sync/sync-fan-out.hxx>
#include <trantor/utils/Logger.h>

#include <optional>

namespace camera_fan_out
{
void handleCameraChange(const Json::Value& json)
{
  if (json.isObject() && json.get("kind", "").asString() == "audit") {
    const auto event = CameraAuditEvent::fromJson(json);
    if (!event) {
      LOG_WARN << "Camera fan-out: dropped malformed audit event";
      return;
    }

    // Drogon loop coroutine: insert first, fan the DB-assigned row out
    // second. The gateway never re-emits the raw camera diff.
    drogon::async_run([event = *event]() -> drogon::Task<void> {
      AuditLogService auditLogService;
      const auto schema = co_await auditLogService.create({
          .recordId = event.recordId,
          .tableName = event.tableName,
          .changes = event.changes,
          .priority = event.priority,
          .createUserId = event.createUserId,
          .eventTimestamp = event.eventTimestamp,
      });

      sync_fan_out::Event fanout;
      fanout.emit.operation = SyncOperation::Log;
      fanout.emit.option = schema.tableName;
      fanout.emit.obj = schema.toJson();
      sync_fan_out::dispatchEvent(fanout);
      co_return;
    });
    return;
  }

  const auto event = sync_fan_out::parseEvent(json);
  if (!event) {
    LOG_WARN << "Camera fan-out: dropped malformed change event";
    return;
  }
  sync_fan_out::dispatchEvent(*event);
}

void subscribeChangeFanOut(NatsBus& bus)
{
  bus.subscribe(
      nats_subject::kSyncChangeWildcard,
      [](std::string_view subject, std::string_view message) {
        // cnats dispatcher thread: marshal the whole handler into the
        // Drogon loop before touching room state.
        drogon::app().getIOLoop(0)->runInLoop(
            [subject = std::string(subject), payload = std::string(message)]() {
              const Json::Value json = json_util::fromString(payload);
              if (subject == nats_subject::kCameraChange)
                handleCameraChange(json);
              else {
                const auto event = sync_fan_out::parseEvent(json);
                if (!event) {
                  LOG_WARN << "Sync fan-out: dropped malformed change event";
                  return;
                }
                sync_fan_out::dispatchEvent(*event);
              }
            });
      });
}
} // namespace camera_fan_out
