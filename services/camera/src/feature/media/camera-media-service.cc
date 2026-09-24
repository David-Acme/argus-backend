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

std::shared_ptr<CameraStreamSink>
CameraMediaService::sinkFor(const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  const auto it = sinks_.find(conn.get());
  return it == sinks_.end() ? nullptr : it->second;
}

void CameraMediaService::storeSink(
    const drogon::WebSocketConnectionPtr& conn,
    const std::shared_ptr<CameraStreamSink>& sink) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  sinks_[conn.get()] = sink;
}

void CameraMediaService::dropSink(
    const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
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
    const std::string quality = payload.get("quality", "main").asString();

    const auto camera = co_await cameraRepository_.findById(cameraId);
    if (!camera)
      throw ResponseException(404, CameraErrors::CameraNotFound);

    auto sink = sinkFor(conn);
    if (!sink) {
      int64_t window = 128 * 1024;
      if (const int64_t v = ConfigService::getInt("streaming.hub_window_bytes");
          v > 0)
        window = v;
      sink = std::make_shared<CameraStreamSink>(conn, window);
      storeSink(conn, sink);
    }
    if (sink->subscriptions() >= maxSubsPerClient_)
      throw ResponseException(429, CameraErrors::TooManyCameraSubscriptions);

    StreamHub::SubscribeInput input;
    input.sink = sink;
    input.cameraId = cameraId;
    input.quality = quality;
    std::string error;
    const uint16_t subId = StreamHub::instance().subscribe(input, error);
    if (subId == 0) {
      const bool viewerLimit = error.rfind("too_many_viewers", 0) == 0;
      auto subscriptionError = viewerLimit ? CameraErrors::TooManyViewers
                                           : CameraErrors::SubscribeFailed;
      if (!error.empty())
        subscriptionError.message = error;
      throw ResponseException(viewerLimit ? 429 : 503, subscriptionError);
    }
    sink->addSubscription();

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
    StreamHub::instance().unsubscribe(subId);
    if (auto sink = sinkFor(conn))
      sink->dropSubscription();
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
