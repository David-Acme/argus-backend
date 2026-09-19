#include "camera-feature-service.hxx"

#include <ctime>
#include <shared/services/stream/camera-source-registrar.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <trantor/utils/Logger.h>

namespace
{
drogon::Task<void> syncSource(CameraSchema camera)
{
  co_await BlockingTask<void>([camera = std::move(camera)] {
    cameraSourceRegistrar().apply(camera);
  });
}

drogon::Task<void> dropSource(int64_t cameraId)
{
  co_await BlockingTask<void>(
      [cameraId] { cameraSourceRegistrar().remove(cameraId); });
}
} // namespace

void CameraFeatureService::emit(SyncOperation operation,
                                const CameraSchema& row) const
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Camera;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = tombstone;
  }
  else {
    body.obj = row.toJson();
  }
  const auto* sink = camera_change::getSink();
  if (!sink) {
    LOG_WARN << "camera change sink not installed; drop camera emit";
    return;
  }
  sink->emitModule(TableName::Camera, body);
}

drogon::Task<CameraSchema>
CameraFeatureService::create(const CreateCameraDto& body) const
{
  const auto row = co_await repository_.create({
      .name = body.name,
      .manufacturer = body.manufacturer,
      .model = body.model,
      .ip = body.ip,
      .port = body.port,
      .username = body.username,
      .password = body.password,
      .cloudUsername = body.cloudUsername,
      .cloudPassword = body.cloudPassword,
      .driver = cameraDriverFromString(body.driver),
      .icon = body.icon,
      .recordMode = cameraRecordModeFromString(body.recordMode),
      .retentionDays = body.retentionDays,
      .capabilities = "[]",
      .config = "{}",
  });
  emit(SyncOperation::Add, row);
  co_await syncSource(row);
  co_return row;
}

drogon::Task<std::optional<CameraSchema>>
CameraFeatureService::update(int64_t id, const UpdateCameraDto& body) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return std::nullopt;

  CameraUpdateInput input;
  input.name = body.name;
  input.manufacturer = body.manufacturer;
  input.model = body.model;
  input.ip = body.ip;
  input.port = body.port;
  input.username = body.username;
  input.password = body.password;
  input.cloudUsername = body.cloudUsername;
  input.cloudPassword = body.cloudPassword;
  input.retentionDays = body.retentionDays;
  input.icon = body.icon;
  if (body.driver)
    input.driver = cameraDriverFromString(*body.driver);
  input.isEnabled = body.isEnabled;
  if (body.recordMode)
    input.recordMode = cameraRecordModeFromString(*body.recordMode);

  const auto row = co_await repository_.update(id, input);
  if (row.id == 0)
    co_return std::nullopt;
  const auto* sink = camera_change::getSink();
  if (!sink) {
    LOG_WARN << "camera change sink not installed; drop camera audit";
  }
  else {
    co_await sink->publishAudit({
        .recordId = row.id,
        .tableName = TableName::Camera,
        .before = existing->toJson(),
        .after = row.toJson(),
        .actorId = std::nullopt,
    });
  }
  co_await syncSource(row);
  co_return row;
}

drogon::Task<bool> CameraFeatureService::remove(int64_t id) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (removed) {
    emit(SyncOperation::Delete, *existing);
    co_await dropSource(id);
  }
  co_return removed;
}
