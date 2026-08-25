#include "zone-feature-service.hxx"

#include <ctime>

void ZoneFeatureService::emit(SyncOperation operation,
                              const ZoneSchema& row) const
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
  socketService_.emitModule(TableName::Zone, body);
}

drogon::Task<std::optional<ZoneSchema>>
ZoneFeatureService::create(const CreateZoneDto& body) const
{
  // The FK would reject a dangling camera anyway; checking here turns that
  // into a 404 instead of a database error.
  const auto camera = co_await cameraRepository_.findById(body.cameraId);
  if (!camera)
    co_return std::nullopt;

  const auto row = co_await repository_.create({
      .cameraId = body.cameraId,
      .name = body.name,
      .points = body.points,
      .zoneType = zoneTypeFromString(body.zoneType),
      .color = body.color,
      .isEnabled = body.isEnabled,
  });
  emit(SyncOperation::Add, row);
  co_return row;
}

drogon::Task<std::optional<ZoneSchema>>
ZoneFeatureService::update(int64_t id, const UpdateZoneDto& body) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return std::nullopt;

  ZoneUpdateInput input;
  input.name = body.name;
  input.points = body.points;
  input.color = body.color;
  input.isEnabled = body.isEnabled;
  if (body.zoneType)
    input.zoneType = zoneTypeFromString(*body.zoneType);

  const auto row = co_await repository_.update(id, input);
  if (row.id == 0)
    co_return std::nullopt;
  co_await syncAuditService_.publishModule({
      .recordId = row.id,
      .tableName = TableName::Zone,
      .before = existing->toJson(),
      .after = row.toJson(),
      .actorId = std::nullopt,
  });
  co_return row;
}

drogon::Task<bool> ZoneFeatureService::remove(int64_t id) const
{
  const auto existing = co_await repository_.findById(id);
  if (!existing)
    co_return false;

  const bool removed = co_await repository_.remove(id);
  if (removed)
    emit(SyncOperation::Delete, *existing);
  co_return removed;
}
