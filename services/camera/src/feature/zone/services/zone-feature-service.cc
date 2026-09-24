#include "zone-feature-service.hxx"

#include <camera/camera-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

namespace
{
SocketEmitDto zoneBody(SyncOperation operation, const ZoneSchema& row)
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Zone;
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
ZoneFeatureService::emit(const ModuleEmitInput& input) const
{
  const auto* sink = camera_change::getSink();
  if (!sink) {
    LOG_WARN << "camera change sink not installed; drop zone emit";
    co_return;
  }
  co_await sink->emitModule(input);
}

drogon::Task<std::optional<ZoneSchema>>
ZoneFeatureService::create(const CreateZoneDto& body) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  ZoneSchema row;
  try {
    const auto camera =
        co_await cameraRepository_.findById(body.cameraId, transaction.get());
    if (!camera) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    row = co_await repository_.create({
        .cameraId = body.cameraId,
        .name = body.name,
        .points = body.points,
        .zoneType = zoneTypeFromString(body.zoneType),
        .color = body.color,
        .isEnabled = body.isEnabled,
        .client = transaction.get(),
    });
    co_await emit({.table = TableName::Zone,
                   .body = zoneBody(SyncOperation::Add, row),
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return row;
}

drogon::Task<std::optional<ZoneSchema>>
ZoneFeatureService::update(int64_t id, const UpdateZoneDto& body) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  ZoneSchema row;
  ZoneSchema before;
  try {
    const auto existing = co_await repository_.findById(id, transaction.get());
    if (!existing) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }
    before = *existing;

    ZoneUpdateInput input;
    input.name = body.name;
    input.points = body.points;
    input.color = body.color;
    input.isEnabled = body.isEnabled;
    if (body.zoneType)
      input.zoneType = zoneTypeFromString(*body.zoneType);
    input.client = transaction.get();

    row = co_await repository_.update(id, input);
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return std::nullopt;
    }

    const auto* sink = camera_change::getSink();
    if (!sink) {
      LOG_WARN << "camera change sink not installed; drop zone audit";
    }
    else {
      co_await sink->publishAudit({
          .recordId = row.id,
          .tableName = TableName::Zone,
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
  co_return row;
}

drogon::Task<bool> ZoneFeatureService::remove(int64_t id) const
{
  auto transaction =
      co_await db_transaction::begin(DbService::cameraClient());
  ZoneSchema before;
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

    co_await emit({.table = TableName::Zone,
                   .body = zoneBody(SyncOperation::Delete, before),
                   .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return true;
}
