#include "camera-sync-rpc-service.hxx"

#include <drogon/drogon.h>
#include <shared/contracts/sync-filter.hxx>
#include <trantor/utils/Logger.h>

namespace
{

// Row scoping is data semantics only; the gateway validated the role.
bool identityPresent(const grpc::CallbackServerContext* context)
{
  bool user = false;
  bool role = false;
  bool device = false;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-user")
      user = true;
    else if (key == "x-argus-role")
      role = true;
    else if (key == "x-argus-device")
      device = true;
  }
  return user && role && device;
}

SyncFilter filterOf(const argus::camera::v1::SyncRange& range)
{
  SyncFilter filter;
  if (range.has_start_time())
    filter.startTime = range.start_time();
  if (range.has_start_id())
    filter.startId = range.start_id();
  if (range.has_end_time())
    filter.endTime = range.end_time();
  return filter;
}

void toProto(const Json::Value& row, argus::camera::v1::CameraRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_name(row["name"].asString());
  out->set_manufacturer(row["manufacturer"].asString());
  out->set_model(row["model"].asString());
  out->set_ip(row["ip"].asString());
  out->set_port(row["port"].asInt());
  out->set_username(row["username"].asString());
  out->set_cloud_username(row["cloudUsername"].asString());
  out->set_driver(row["driver"].asString());
  out->set_icon(row["icon"].asString());
  out->set_record_mode(row["recordMode"].asString());
  if (!row["retentionDays"].isNull())
    out->set_retention_days(row["retentionDays"].asInt64());
  out->set_capabilities(row["capabilities"].asString());
  out->set_config(row["config"].asString());
  out->set_is_enabled(row["isEnabled"].asBool());
  out->set_is_online(row["isOnline"].asBool());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row, argus::camera::v1::ZoneRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_camera_id(row["cameraId"].asInt64());
  out->set_name(row["name"].asString());
  out->set_points(row["points"].asString());
  out->set_zone_type(row["zoneType"].asString());
  out->set_color(row["color"].asString());
  out->set_is_enabled(row["isEnabled"].asBool());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row, argus::camera::v1::CameraStreamRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_camera_id(row["cameraId"].asInt64());
  out->set_label(row["label"].asString());
  out->set_url(row["url"].asString());
  out->set_resolution(row["resolution"].asString());
  out->set_fps(row["fps"].asInt());
  out->set_codec(row["codec"].asString());
  out->set_is_primary(row["isPrimary"].asBool());
  out->set_is_enabled(row["isEnabled"].asBool());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

template <typename TableRows, typename Repo>
drogon::Task<void>
fill(const argus::camera::v1::TablePull& body, TableRows* rows,
     const Repo& repo)
{
  if (body.required_create()) {
    const auto data = co_await repo.find(filterOf(body.created()));
    for (const auto& row : data)
      toProto(row, rows->add_created());
  }
  if (body.required_deleted()) {
    const auto data = co_await repo.findDeleted(filterOf(body.deleted()));
    for (const auto& row : data) {
      auto* tombstone = rows->add_deleted();
      tombstone->set_id(row["id"].asInt64());
      tombstone->set_deleted_at(row["deletedAt"].asInt64());
    }
  }
  if (body.find_last_created()) {
    const auto row = co_await repo.findLast({});
    if (row)
      toProto(*row, rows->mutable_last_created());
  }
  if (body.find_last_deleted()) {
    const auto row = co_await repo.findLastDeleted({});
    if (row) {
      rows->mutable_last_deleted()->set_id((*row)["id"].asInt64());
      rows->mutable_last_deleted()->set_deleted_at(
          (*row)["deletedAt"].asInt64());
    }
  }
  co_return;
}

} // namespace

grpc::ServerUnaryReactor* CameraSyncRpcService::PullTable(
    grpc::CallbackServerContext* context,
    const argus::camera::v1::PullTableRequest* request,
    argus::camera::v1::PullTableResponse* response)
{
  if (!identityPresent(context)) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "identity metadata missing"));
    return reactor;
  }
  if (!request->has_camera() && !request->has_camera_stream() &&
      !request->has_zone()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "table branch is required"));
    return reactor;
  }

  const argus::camera::v1::PullTableRequest pull = *request;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, pull, responseWriter]() {
    drogon::async_run([this, reactor, pull,
                       responseWriter]() -> drogon::Task<void> {
      try {
        switch (pull.table_case()) {
          case argus::camera::v1::PullTableRequest::kCamera:
            co_await fill(pull.camera(), responseWriter->mutable_camera(),
                          cameras_);
            break;
          case argus::camera::v1::PullTableRequest::kCameraStream:
            co_await fill(pull.camera_stream(),
                          responseWriter->mutable_camera_stream(), streams_);
            break;
          case argus::camera::v1::PullTableRequest::kZone:
            co_await fill(pull.zone(), responseWriter->mutable_zone(), zones_);
            break;
          default:
            co_return;
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera sync RPC: PullTable failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
      }
      co_return;
    });
  });
  return reactor;
}
