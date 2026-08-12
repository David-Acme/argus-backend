#include "sync-service.hxx"

#include <algorithm>
#include <config/app-config.hxx>
#include <mutex>
#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <unordered_map>

SyncService::SyncService()
{
  if (const int v = ConfigService::getInt("streaming.hub_max_subs_per_client");
      v > 0)
    maxSubsPerClient_ = v;
}

std::shared_ptr<DrogonStreamSink>
SyncService::sinkFor(const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  const auto it = sinks_.find(conn.get());
  return it == sinks_.end() ? nullptr : it->second;
}

void SyncService::storeSink(const drogon::WebSocketConnectionPtr& conn,
                            const std::shared_ptr<DrogonStreamSink>& sink) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  sinks_[conn.get()] = sink;
}

void SyncService::dropSink(const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  sinks_.erase(conn.get());
}

drogon::Task<void>
SyncService::handleConnect(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) const
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  conn->setContext(std::make_shared<JwtContext>(ctx));

  std::vector<RoomId> rooms;
  for (const auto table : role_access::readableTables(ctx.role))
    rooms.push_back(moduleRoom(table));
  rooms.push_back(userRoom(ctx.sub));
  roomManager_.joinMany(rooms, conn);

  Json::Value user;
  user["id"] = ctx.sub;
  user["role"] = userRoleToString(ctx.role);
  user["isActive"] = ctx.isActive;

  SocketEmitDto response;
  response.operation = SyncOperation::InitialInfo;
  response.obj = user;
  conn->sendJson(response.toJson());
  co_return;
}

drogon::Task<void>
SyncService::handleMessage(const drogon::WebSocketConnectionPtr& conn,
                           const Json::Value& obj) const
{
  if (!obj.isMember("type") || !obj["type"].isString())
    throw ResponseException("Missing message type", 400,
                            AppConfig::ERROR_CODE_BAD_REQUEST);

  const std::string type = obj["type"].asString();
  const Json::Value& payload = obj["payload"];
  const auto& ctx = conn->getContextRef<JwtContext>();

  if (type == "sync") {
    const auto dto = SynchronizedDto::fromJson(payload);
    conn->sendJson(co_await synchronizedService_.sync(dto, ctx));
    co_return;
  }
  if (type == "sync_audit_log") {
    const auto dto = SynchronizedLogDto::fromJson(payload);
    conn->sendJson(co_await synchronizedService_.syncAuditLog(dto, ctx));
    co_return;
  }
  if (type == "sync_user_audit_log") {
    const auto dto = SynchronizedLogDto::fromJson(payload);
    conn->sendJson(co_await synchronizedService_.syncUserAuditLog(dto, ctx));
    co_return;
  }

  if (type == "camera:subscribe") {
    if (!role_access::hasAccess(ctx.role, TableName::Camera,
                                RolePermission::Read))
      throw ResponseException("Forbidden", 403,
                              AppConfig::ERROR_CODE_FORBIDDEN);

    const int64_t cameraId = payload.get("cameraId", 0).asInt64();
    if (cameraId <= 0)
      throw ResponseException("Invalid cameraId", 400,
                              AppConfig::ERROR_CODE_BAD_REQUEST);
    const std::string quality = payload.get("quality", "main").asString();

    auto sink = sinkFor(conn);
    if (!sink) {
      int64_t window = 128 * 1024;
      if (const int64_t v = ConfigService::getInt("streaming.hub_window_bytes");
          v > 0)
        window = v;
      sink = std::make_shared<DrogonStreamSink>(conn, window);
      storeSink(conn, sink);
    }
    if (sink->subscriptions() >= maxSubsPerClient_)
      throw ResponseException("Too many camera subscriptions", 429,
                              AppConfig::ERROR_CODE_TOO_MANY_REQUESTS);

    StreamHub::SubscribeInput input;
    input.sink = sink;
    input.cameraId = cameraId;
    input.quality = quality;
    std::string error;
    const uint16_t subId = StreamHub::instance().subscribe(input, error);
    if (subId == 0)
      throw ResponseException(error.empty() ? "subscribe_failed" : error, 503,
                              AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
    sink->addSubscription();

    Json::Value resp;
    resp["subId"] = subId;
    resp["mime"] = "video/mp4";
    Json::Value out;
    out["type"] = "camera:ready";
    out["payload"] = resp;
    conn->sendJson(out);
    co_return;
  }

  if (type == "camera:ack") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    const int64_t bytes = payload.get("bytes", 0).asInt64();
    StreamHub::instance().ack(subId, bytes);
    co_return;
  }

  if (type == "camera:unsubscribe") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    StreamHub::instance().unsubscribe(subId);
    if (auto sink = sinkFor(conn))
      sink->dropSubscription();
    co_return;
  }

  throw ResponseException("Unknown message type", 400,
                          AppConfig::ERROR_CODE_BAD_REQUEST);
}

void SyncService::handleDisconnect(
    const drogon::WebSocketConnectionPtr& conn) const
{
  if (auto sink = sinkFor(conn))
    StreamHub::instance().closeAll(sink.get());
  dropSink(conn);
  roomManager_.leaveAll(conn);
  conn->clearContext();
}
