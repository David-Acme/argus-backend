#include "sync-service.hxx"

#include <config/app-config.hxx>
#include <mutex>
#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <unordered_map>

namespace
{

class DrogonStreamSink final : public StreamHub::ISink
{
public:
  explicit DrogonStreamSink(drogon::WebSocketConnectionPtr conn)
      : conn_(std::move(conn))
  {
  }

  void setSubId(uint16_t subId) { subId_ = subId; }

  bool sendBinary(const uint8_t* data, size_t len) override
  {
    if (!conn_ || conn_->disconnected())
      return false;
    conn_->send(reinterpret_cast<const char*>(data), len,
                drogon::WebSocketMessageType::Binary);
    return true;
  }

  void onClosed(const std::string& reason) override
  {
    if (!conn_ || conn_->disconnected())
      return;
    Json::Value j;
    j["type"] = "camera:closed";
    j["payload"]["subId"] = subId_;
    j["payload"]["reason"] = reason;
    conn_->sendJson(j);
  }

private:
  drogon::WebSocketConnectionPtr conn_;
  uint16_t subId_{0};
};

std::mutex gSinksMutex;
std::unordered_map<const void*, std::shared_ptr<DrogonStreamSink>> gSinks;

std::shared_ptr<DrogonStreamSink>
sinkFor(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(gSinksMutex);
  const auto it = gSinks.find(conn.get());
  return it == gSinks.end() ? nullptr : it->second;
}

void storeSink(const drogon::WebSocketConnectionPtr& conn,
               const std::shared_ptr<DrogonStreamSink>& sink)
{
  std::lock_guard<std::mutex> lock(gSinksMutex);
  gSinks[conn.get()] = sink;
}

void dropSink(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(gSinksMutex);
  gSinks.erase(conn.get());
}

} // namespace

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
      sink = std::make_shared<DrogonStreamSink>(conn);
      storeSink(conn, sink);
    }

    StreamHub::SubscribeInput input;
    input.sink = sink;
    input.cameraId = cameraId;
    input.quality = quality;
    std::string error;
    const uint16_t subId = StreamHub::subscribe(input, error);
    if (subId == 0)
      throw ResponseException(error.empty() ? "subscribe_failed" : error, 503,
                              "SERVICE_UNAVAILABLE");
    sink->setSubId(subId);

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
    StreamHub::ack(subId, bytes);
    co_return;
  }

  if (type == "camera:unsubscribe") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    StreamHub::unsubscribe(subId);
    co_return;
  }

  throw ResponseException("Unknown message type", 400,
                          AppConfig::ERROR_CODE_BAD_REQUEST);
}

void SyncService::handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const
{
  if (auto sink = sinkFor(conn))
    StreamHub::closeAll(sink.get());
  dropSink(conn);
  roomManager_.leaveAll(conn);
  conn->clearContext();
}
