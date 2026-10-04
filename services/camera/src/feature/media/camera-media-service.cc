#include "camera-media-service.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-access.hxx>
#include <config/config-service.hxx>

CameraMediaService::CameraMediaService()
{
  if (const int v = ConfigService::getInt("streaming.hub_max_subs_per_client");
      v > 0)
    maxSubsPerClient_ = v;
}

int64_t CameraMediaService::streamWindowBytes()
{
  constexpr int64_t kDefaultWindowBytes = 128 * 1024;
  const int64_t configured = ConfigService::getInt("streaming.hub_window_bytes");
  return configured > 0 ? configured : kDefaultWindowBytes;
}

std::shared_ptr<CameraStreamSink>
CameraMediaService::sinkFor(const drogon::WebSocketConnectionPtr& conn) const
{
  std::scoped_lock lock(sinksMutex_);
  const auto it = sinks_.find(conn.get());
  return it == sinks_.end() ? nullptr : it->second;
}

void CameraMediaService::storeSink(
    const drogon::WebSocketConnectionPtr& conn,
    const std::shared_ptr<CameraStreamSink>& sink) const
{
  std::scoped_lock lock(sinksMutex_);
  sinks_[conn.get()] = sink;
}

void CameraMediaService::dropSink(
    const drogon::WebSocketConnectionPtr& conn) const
{
  std::scoped_lock lock(sinksMutex_);
  sinks_.erase(conn.get());
}

drogon::Task<bool> CameraMediaService::handleText(const SyncFrameInput& input)
{
  const drogon::WebSocketConnectionPtr& conn = input.conn;
  const Json::Value& message = input.message;
  (void)input.raw;
  const std::string type = message["type"].asString();
  const Json::Value& payload = message["payload"];
  const auto& ctx = conn->getContextRef<JwtContext>();

  if (type == "camera:subscribe") {
    if (!role_access::hasAccess(
            {.role = ctx.role,
             .table = TableName::Camera,
             .perm = RolePermission::Read}))
      throw ResponseException(403, CameraErrors::Forbidden);

    const int64_t cameraId = payload.get("cameraId", 0).asInt64();
    if (cameraId <= 0)
      throw ResponseException(400, CameraErrors::InvalidCameraId);
    const CameraStream stream =
        cameraStreamFromString(payload.get("quality", "").asString())
            .value_or(camera_stream_role::streamFor(CameraStreamRole::LiveView));

    const auto camera = co_await cameraRepository_.findById(cameraId);
    if (!camera)
      throw ResponseException(404, CameraErrors::CameraNotFound);
    if (conn->disconnected())
      co_return true;

    auto sink = sinkFor(conn);
    if (!sink) {
      sink = std::make_shared<CameraStreamSink>(conn, streamWindowBytes());
      storeSink(conn, sink);
    }
    if (StreamHub::instance().subscriptionsOf(sink.get()) >= maxSubsPerClient_)
      throw ResponseException(429, CameraErrors::TooManyCameraSubscriptions);

    std::string error;
    const uint16_t subId = StreamHub::instance().subscribe(
        {.sink = sink,
         .cameraId = cameraId,
         .stream = stream,
         .fastStart = payload["fastStart"].isBool() && payload["fastStart"].asBool()},
        error);
    if (subId == 0) {
      const bool viewerLimit = error.rfind("too_many_viewers", 0) == 0;
      auto subscriptionError = viewerLimit ? CameraErrors::TooManyViewers
                                           : CameraErrors::SubscribeFailed;
      if (!error.empty())
        subscriptionError.message = error;
      throw ResponseException(viewerLimit ? 429 : 503, subscriptionError);
    }

    Json::Value resp;
    resp["subId"] = subId;
    resp["mime"] = "video/mp4";
    Json::Value out;
    out["type"] = "camera:ready";
    out["payload"] = resp;
    conn->sendJson(out);
    co_return true;
  }

  if (type == "camera:ack") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    const int64_t bytes = payload.get("bytes", 0).asInt64();
    StreamHub::instance().ack(subId, bytes);
    co_return true;
  }

  if (type == "camera:unsubscribe") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    if (auto sink = sinkFor(conn))
      StreamHub::instance().unsubscribe(subId, sink.get());
    co_return true;
  }

  co_return false;
}

void CameraMediaService::handleClose(const drogon::WebSocketConnectionPtr& conn)
{
  if (auto sink = sinkFor(conn))
    StreamHub::instance().closeAll(sink.get());
  dropSink(conn);
}
