#include "sync-service.hxx"

#include <config/app-config.hxx>
#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/exceptions/response-exception.hxx>

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

  throw ResponseException("Unknown message type", 400,
                          AppConfig::ERROR_CODE_BAD_REQUEST);
}

void SyncService::handleDisconnect(const drogon::WebSocketConnectionPtr& conn) const
{
  roomManager_.leaveAll(conn);
  conn->clearContext();
}
