#include "camera-sync-source.hxx"

#include <config/app-config.hxx>
#include <json/value.h>
#include <shared/contracts/sync-filter.hxx>
#include <shared/enums.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <string>
#include <utility>
#include <vector>

namespace
{
SyncIdentity identityFor(const JwtContext& ctx)
{
  return SyncIdentity{.userId = ctx.sub,
                      .role = userRoleToString(ctx.role),
                      .device = ctx.deviceHash};
}

argus::camera::v1::SyncRange toRange(const SyncFilter& filter)
{
  argus::camera::v1::SyncRange range;
  if (filter.startTime)
    range.set_start_time(*filter.startTime);
  if (filter.startId)
    range.set_start_id(*filter.startId);
  if (filter.endTime)
    range.set_end_time(*filter.endTime);
  return range;
}

argus::camera::v1::TablePull* mutablePull(argus::camera::v1::PullTableRequest& request)
{
  switch (request.table_case()) {
    case argus::camera::v1::PullTableRequest::kCamera:
      return request.mutable_camera();
    case argus::camera::v1::PullTableRequest::kCameraStream:
      return request.mutable_camera_stream();
    case argus::camera::v1::PullTableRequest::kZone:
      return request.mutable_zone();
    default:
      return nullptr;
  }
}

Json::Value cameraRowToJson(const argus::camera::v1::CameraRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["name"] = row.name();
  json["manufacturer"] = row.manufacturer();
  json["model"] = row.model();
  json["ip"] = row.ip();
  json["port"] = row.port();
  json["username"] = row.username();
  json["cloudUsername"] = row.cloud_username();
  json["driver"] = row.driver();
  json["icon"] = row.icon();
  json["recordMode"] = row.record_mode();
  json["retentionDays"] = row.has_retention_days()
                              ? Json::Value(Json::Int64(row.retention_days()))
                              : Json::Value();
  json["capabilities"] = row.capabilities();
  json["config"] = row.config();
  json["isEnabled"] = row.is_enabled();
  json["isOnline"] = row.is_online();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value zoneRowToJson(const argus::camera::v1::ZoneRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["cameraId"] = Json::Int64(row.camera_id());
  json["name"] = row.name();
  json["points"] = row.points();
  json["zoneType"] = row.zone_type();
  json["color"] = row.color();
  json["isEnabled"] = row.is_enabled();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value streamRowToJson(const argus::camera::v1::CameraStreamRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["cameraId"] = Json::Int64(row.camera_id());
  json["label"] = row.label();
  json["url"] = row.url();
  json["resolution"] = row.resolution();
  json["fps"] = row.fps();
  json["codec"] = row.codec();
  json["isPrimary"] = row.is_primary();
  json["isEnabled"] = row.is_enabled();
  json["createdAt"] = Json::Int64(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(Json::Int64(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(Json::Int64(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value deletedRowToJson(const argus::camera::v1::DeletedRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["deletedAt"] = Json::Int64(row.deleted_at());
  return json;
}

ResponseException unavailable()
{
  return ResponseException("Camera sync unavailable", 503,
                           AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
}
} // namespace

class CameraSyncGateway::Pull : public Syncable
{
public:
  Pull(std::shared_ptr<CameraSyncClient> client, CameraSyncTable table,
       SyncIdentity identity)
      : client_(std::move(client)), table_(table), identity_(std::move(identity))
  {
  }

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override
  {
    const auto response = co_await pull(table_, Mode::Created, filter);
    co_return createdRows(response);
  }

  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override
  {
    const auto response = co_await pull(table_, Mode::Deleted, filter);
    co_return deletedRows(response);
  }

  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter&) const override
  {
    const auto response = co_await pull(table_, Mode::LastCreated, {});
    std::optional<Json::Value> row;
    switch (response.table_case()) {
      case argus::camera::v1::PullTableResponse::kCamera:
        if (response.camera().has_last_created())
          row = cameraRowToJson(response.camera().last_created());
        break;
      case argus::camera::v1::PullTableResponse::kCameraStream:
        if (response.camera_stream().has_last_created())
          row = streamRowToJson(response.camera_stream().last_created());
        break;
      case argus::camera::v1::PullTableResponse::kZone:
        if (response.zone().has_last_created())
          row = zoneRowToJson(response.zone().last_created());
        break;
      default:
        throw unavailable();
    }
    co_return row;
  }

  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter&) const override
  {
    const auto response = co_await pull(table_, Mode::LastDeleted, {});
    std::optional<Json::Value> row;
    switch (response.table_case()) {
      case argus::camera::v1::PullTableResponse::kCamera:
        if (response.camera().has_last_deleted())
          row = deletedRowToJson(response.camera().last_deleted());
        break;
      case argus::camera::v1::PullTableResponse::kCameraStream:
        if (response.camera_stream().has_last_deleted())
          row = deletedRowToJson(response.camera_stream().last_deleted());
        break;
      case argus::camera::v1::PullTableResponse::kZone:
        if (response.zone().has_last_deleted())
          row = deletedRowToJson(response.zone().last_deleted());
        break;
      default:
        throw unavailable();
    }
    co_return row;
  }

private:
  enum class Mode
  {
    Created,
    Deleted,
    LastCreated,
    LastDeleted,
  };

  static argus::camera::v1::PullTableRequest
  requestFor(CameraSyncTable table, Mode mode, const SyncFilter& filter)
  {
    argus::camera::v1::PullTableRequest request;
    switch (table) {
      case CameraSyncTable::Camera:
        request.mutable_camera();
        break;
      case CameraSyncTable::CameraStream:
        request.mutable_camera_stream();
        break;
      case CameraSyncTable::Zone:
        request.mutable_zone();
        break;
    }
    auto* body = mutablePull(request);
    switch (mode) {
      case Mode::Created:
        body->set_required_create(true);
        *body->mutable_created() = toRange(filter);
        break;
      case Mode::Deleted:
        body->set_required_deleted(true);
        *body->mutable_deleted() = toRange(filter);
        break;
      case Mode::LastCreated:
        body->set_find_last_created(true);
        break;
      case Mode::LastDeleted:
        body->set_find_last_deleted(true);
        break;
    }
    return request;
  }

  drogon::Task<argus::camera::v1::PullTableResponse>
  pull(CameraSyncTable table, Mode mode, const SyncFilter& filter) const
  {
    const auto request = requestFor(table, mode, filter);
    auto response = co_await BlockingTask<
        std::optional<argus::camera::v1::PullTableResponse>>(
        [this, request]() { return client_->pullTable(request, identity_); });
    if (!response)
      throw unavailable();
    co_return std::move(*response);
  }

  static std::vector<Json::Value>
  createdRows(const argus::camera::v1::PullTableResponse& response)
  {
    std::vector<Json::Value> rows;
    switch (response.table_case()) {
      case argus::camera::v1::PullTableResponse::kCamera:
        for (const auto& row : response.camera().created())
          rows.push_back(cameraRowToJson(row));
        break;
      case argus::camera::v1::PullTableResponse::kCameraStream:
        for (const auto& row : response.camera_stream().created())
          rows.push_back(streamRowToJson(row));
        break;
      case argus::camera::v1::PullTableResponse::kZone:
        for (const auto& row : response.zone().created())
          rows.push_back(zoneRowToJson(row));
        break;
      default:
        throw unavailable();
    }
    return rows;
  }

  static std::vector<Json::Value>
  deletedRows(const argus::camera::v1::PullTableResponse& response)
  {
    std::vector<Json::Value> rows;
    switch (response.table_case()) {
      case argus::camera::v1::PullTableResponse::kCamera:
        for (const auto& row : response.camera().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      case argus::camera::v1::PullTableResponse::kCameraStream:
        for (const auto& row : response.camera_stream().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      case argus::camera::v1::PullTableResponse::kZone:
        for (const auto& row : response.zone().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      default:
        throw unavailable();
    }
    return rows;
  }

  std::shared_ptr<CameraSyncClient> client_;
  CameraSyncTable table_;
  SyncIdentity identity_;
};

CameraSyncGateway::CameraSyncGateway(std::string target)
    : client_(std::make_shared<CameraSyncClient>(std::move(target)))
{
}

bool CameraSyncGateway::serves(CameraSyncTable) const
{
  return true;
}

std::unique_ptr<Syncable>
CameraSyncGateway::sourceFor(CameraSyncTable table, const JwtContext& ctx) const
{
  return std::make_unique<Pull>(client_, table, identityFor(ctx));
}
