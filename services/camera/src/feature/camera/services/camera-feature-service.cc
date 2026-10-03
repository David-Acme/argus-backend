#include "camera-feature-service.hxx"

#include <camera/camera-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/stream/camera-source-registrar.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <runtime/blocking-task.hxx>
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

SocketEmitDto cameraBody(SyncOperation operation, const CameraSchema& row)
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
  return body;
}
}

drogon::Task<void>
CameraFeatureService::emit(const ModuleEmitInput& input) const
{
  const auto* sink = camera_change::getSink();
  if (!sink) {
    LOG_WARN << "camera change sink not installed; drop camera emit";
    co_return;
  }
  co_await sink->emitModule(input);
}

drogon::Task<CameraSchema>
CameraFeatureService::create(const CreateCameraDto& body) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  CameraSchema row;
  try {
    row = co_await repository_.create({
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
        .client = transaction.get(),
    });
    co_await emit({.table = TableName::Camera,
                   .body = cameraBody(SyncOperation::Add, row),
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await syncSource(row);
  co_return row;
}

drogon::Task<std::optional<CameraSchema>>
CameraFeatureService::update(int64_t id, const UpdateCameraDto& body) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  CameraSchema row;
  CameraSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = *existing;

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
    input.client = transaction.get();

    row = co_await repository_.update(id, input);
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    const auto* sink = camera_change::getSink();
    if (!sink) {
      LOG_WARN << "camera change sink not installed; drop camera audit";
    }
    else {
      co_await sink->publishAudit({
          .recordId = row.id,
          .tableName = TableName::Camera,
          .before = before.toJson(),
          .after = row.toJson(),
          .actorId = std::nullopt,
          .client = transaction.get(),
      });
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  CameraDriverRegistry::instance().forget(id);
  if (before.ip != row.ip || before.port != row.port)
    CameraSceneLog::instance().noteAimed(
        id, static_cast<int64_t>(std::time(nullptr)) * 1000);
  co_await syncSource(row);
  co_return row;
}

drogon::Task<bool> CameraFeatureService::remove(int64_t id) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  CameraSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    before = *existing;

    if (!co_await repository_.remove(id, transaction.get())) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    co_await emit({.table = TableName::Camera,
                   .body = cameraBody(SyncOperation::Delete, before),
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  CameraDriverRegistry::instance().forget(id);
  CameraSceneLog::instance().forget(id);
  SnapshotStore::instance().forget(id);
  co_await dropSource(id);
  co_return true;
}
