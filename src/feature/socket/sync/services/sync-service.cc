#include "sync-service.hxx"

#include <config/app-config.hxx>
#include <shared/access/role-access.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/exceptions/response-exception.hxx>

drogon::Task<void>
SyncService::refreshContext(const drogon::WebSocketConnectionPtr& conn) const
{
  auto& ctx = conn->getContextRef<JwtContext>();
  const auto user = co_await userRepository_.findById(ctx.sub);
  if (!user || !user->isActive)
    throw ResponseException("User account is disabled", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  ctx.name = user->name + " " + user->lastName;
  ctx.role = user->role;
  ctx.isActive = user->isActive;
  co_return;
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

  if (forwarder_)
    forwarder_->onConnect(req, conn);
  co_return;
}

drogon::Task<void>
SyncService::handleMessage(const drogon::WebSocketConnectionPtr& conn,
                           const Json::Value& obj,
                           std::string_view rawMessage) const
{
  if (!obj.isMember("type") || !obj["type"].isString())
    throw ResponseException("Missing message type", 400,
                            AppConfig::ERROR_CODE_BAD_REQUEST);

  co_await refreshContext(conn);

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

  if (type.rfind("camera:", 0) == 0 || type.rfind("voice:", 0) == 0) {
    const bool handled = forwarder_ &&
                         co_await forwarder_->forwardText(conn, obj, rawMessage);
    if (!handled)
      throw ResponseException("Unknown message type", 400,
                              AppConfig::ERROR_CODE_BAD_REQUEST);
    co_return;
  }

  throw ResponseException("Unknown message type", 400,
                          AppConfig::ERROR_CODE_BAD_REQUEST);
}

void SyncService::handleBinary(const drogon::WebSocketConnectionPtr& conn,
                               const std::string& data) const
{
  if (forwarder_)
    forwarder_->forwardBinary(conn, data);
}

void SyncService::handleDisconnect(
    const drogon::WebSocketConnectionPtr& conn) const
{
  if (forwarder_)
    forwarder_->onClose(conn);
  roomManager_.leaveAll(conn);
  conn->clearContext();
}

void SyncService::setForwarder(std::shared_ptr<SyncForwarder> forwarder)
{
  forwarder_ = std::move(forwarder);
}