#include "sync-service.hxx"

#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <auth/module-gate.hxx>
#include <auth/request-context.hxx>
#include <auth/role-access.hxx>
#include <sync/sync-operation.hxx>
#include <auth/user-directory.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-errors.hxx>
#include <shared/services/context/user-context.hxx>

#include <trantor/utils/Logger.h>

#include <exception>
#include <utility>

SyncService::SyncService() : lanes_(std::make_shared<ConnectionLanes>())
{
  setLanes(lanes_);
}

void SyncService::setLanes(std::shared_ptr<ConnectionLanes> lanes)
{
  if (!lanes)
    return;
  lanes_ = std::move(lanes);
  lanes_->setDrainStarter(
      [this](const drogon::WebSocketConnectionPtr& conn,
             const std::shared_ptr<FrameLane>& lane) { startDrain(conn, lane); });
}

std::shared_ptr<FrameLane>
SyncService::laneFor(const drogon::WebSocketConnectionPtr& conn) const
{
  if (auto lane = lanes_->find(conn))
    return lane;
  const int64_t userId =
      conn->hasContext() ? conn->getContextRef<JwtContext>().sub : 0;
  return lanes_->open(
      {.conn = conn,
       .userId = userId,
       .loop = trantor::EventLoop::getEventLoopOfCurrentThread()});
}

void SyncService::startDrain(const drogon::WebSocketConnectionPtr& conn,
                             const std::shared_ptr<FrameLane>& lane) const
{
  drogon::async_run([this, conn, lane]() -> drogon::Task<> {
    for (auto job = lane->next(); job.has_value(); job = lane->next()) {
      if (conn->disconnected())
        continue;
      if (job->kind == FrameJobKind::Revalidate)
        co_await revalidate(conn);
      else
        co_await runFrame(conn, std::move(*job));
    }
  });
}

drogon::Task<void> SyncService::runFrame(const drogon::WebSocketConnectionPtr& conn,
                                         FrameJob job) const
{
  const std::string type = std::move(job.type);
  try {
    co_await handleMessage({.conn = conn, .message = job.message, .raw = job.raw});
  }
  catch (const ValidationException& ex) {
    sendSocketFrameError(
        {.conn = conn, .type = type, .status = 422, .error = ex.what()});
  }
  catch (const ResponseException& ex) {
    sendSocketFrameError({.conn = conn,
                          .type = type,
                          .status = ex.statusCode(),
                          .error = ex.what()});
    if (ex.statusCode() == SyncErrors::UserAccountDisabled.status)
      closeDisabled(conn);
  }
  catch (const std::exception& ex) {
    LOG_ERROR << "SyncSocket: " << type << " failed: " << ex.what();
    sendSocketFrameError(
        {.conn = conn,
         .type = type,
         .status = SyncErrors::FrameFailed.status,
         .error = std::string(SyncErrors::FrameFailed.message)});
  }
}

drogon::Task<void>
SyncService::revalidate(const drogon::WebSocketConnectionPtr& conn) const
{
  try {
    co_await refreshContext(conn);
  }
  catch (const ResponseException& ex) {
    if (ex.statusCode() == SyncErrors::UserAccountDisabled.status)
      closeDisabled(conn);
  }
  catch (const std::exception& ex) {
    LOG_WARN << "Sync: socket revalidation failed: " << ex.what();
  }
}

void SyncService::closeDisabled(const drogon::WebSocketConnectionPtr& conn) const
{
  const auto lane = lanes_->find(conn);
  trantor::EventLoop* loop = lane ? lane->loop() : nullptr;
  const auto close = [conn]() {
    RoomManager{}.leaveAll(conn);
    if (conn->connected())
      conn->shutdown(drogon::CloseCode::kViolation, "account_disabled");
  };
  if (loop)
    loop->runInLoop(close);
  else
    close();
}

drogon::Task<void>
SyncService::refreshContext(const drogon::WebSocketConnectionPtr& conn) const
{
  auto& ctx = conn->getContextRef<JwtContext>();

  DirectoryLookup found;
  if (userDirectory_)
    found = co_await userDirectory_->lookup(ctx.sub);
  if (userDirectory_ && found.status == DirectoryLookupStatus::Unavailable)
    throw ResponseException(SyncErrors::IdentitySyncUnavailable);

  const auto& resolved = found.user;
  if (!resolved || !resolved->isActive)
    throw ResponseException(401, SyncErrors::UserAccountDisabled);

  ctx.name = resolved->name + " " + resolved->lastName;
  const bool roleChanged = resolved->role != ctx.role;
  if (roleChanged) {
    roomManager_.replaceRoleRooms(
        {.userId = ctx.sub, .oldRole = ctx.role, .newRole = resolved->role});
    userContext().userChanged(ctx.sub, resolved->role);
  }
  ctx.role = resolved->role;
  ctx.isActive = resolved->isActive;
  co_return;
}

drogon::Task<void>
SyncService::handleConnect(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) const
{
  const JwtContext ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  conn->setContext(std::make_shared<JwtContext>(ctx));
  lanes_->open({.conn = conn,
                .userId = ctx.sub,
                .loop = trantor::EventLoop::getEventLoopOfCurrentThread()});

  const Json::Value ownerCatalog = co_await userContext().ownerCatalogFor(ctx.role);
  if (!conn->connected())
    co_return;
  const bool settled = moduleGate().settled();
  const ModuleSnapshot modules = moduleGate().snapshot();
  const UserRole role = conn->getContextRef<JwtContext>().role;

  std::vector<RoomId> rooms = roleRoomsOf(role, modules);
  rooms.push_back(userRoom(ctx.sub));
  rooms.push_back(kConnectedRoom);
  roomManager_.joinMany(rooms, conn);

  Json::Value user;
  user["id"] = ctx.sub;
  user["role"] = userRoleToString(role);
  user["isActive"] = ctx.isActive;
  if (settled)
    user["context"] = user_context::build(
        {.userId = ctx.sub, .role = role, .modules = modules, .ownerCatalog = ownerCatalog});

  SocketEmitDto response;
  response.operation = SyncOperation::InitialInfo;
  response.obj = std::move(user);
  conn->sendJson(response.toJson());
  sendHeartbeat(conn, ctx.sub);

  if (forwarder_)
    forwarder_->onConnect(req, conn);
  co_return;
}

drogon::Task<void>
SyncService::handleMessage(const SyncFrameInput& input) const
{
  const drogon::WebSocketConnectionPtr& conn = input.conn;
  const Json::Value& obj = input.message;

  if (!obj.isMember("type") || !obj["type"].isString())
    throw ResponseException(400, SyncErrors::MissingMessageType);

  co_await refreshContext(conn);

  const std::string type = obj["type"].asString();
  const Json::Value& payload = obj["payload"];
  if (!payload.isNull() && !payload.isObject())
    throw ResponseException(400, SyncErrors::InvalidPayload);
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

  if (type == "heartbeat") {
    if (!heartbeatSource_)
      throw ResponseException(400, SyncErrors::UnknownMessageType);
    sendHeartbeat(conn, ctx.sub);
    co_return;
  }

  if (type.starts_with("camera:") || type.starts_with("voice:")) {
    const bool handled = forwarder_ && co_await forwarder_->forwardText(input);
    if (!handled)
      throw ResponseException(400, SyncErrors::UnknownMessageType);
    co_return;
  }

  throw ResponseException(400, SyncErrors::UnknownMessageType);
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
  lanes_->close(conn);
}

void SyncService::sendHeartbeat(const drogon::WebSocketConnectionPtr& conn,
                                int64_t userId) const
{
  if (!heartbeatSource_)
    return;
  SocketEmitDto frame;
  frame.operation = SyncOperation::Heartbeat;
  frame.option = TableName::User;
  frame.obj = heartbeatSource_->heartbeatFor(userId);
  conn->sendJson(frame.toJson());
}

void SyncService::setHeartbeatSource(
    std::shared_ptr<const HeartbeatSource> source)
{
  heartbeatSource_ = std::move(source);
}

void SyncService::setForwarder(std::shared_ptr<SyncForwarder> forwarder)
{
  forwarder_ = std::move(forwarder);
}

void SyncService::setCameraSource(std::shared_ptr<CameraSyncSource> source)
{
  cameraSource_ = std::move(source);
  synchronizedService_.setCameraSource(cameraSource_.get());
}

void SyncService::setProductivitySource(
    std::shared_ptr<ProductivitySyncSource> source)
{
  productivitySource_ = std::move(source);
  synchronizedService_.setProductivitySource(productivitySource_.get());
}

void SyncService::setNotificationSource(
    std::shared_ptr<NotificationSyncSource> source)
{
  notificationSource_ = std::move(source);
  synchronizedService_.setNotificationSource(notificationSource_.get());
}

void SyncService::setIdentitySource(std::shared_ptr<IdentitySyncSource> source)
{
  identitySyncSource_ = std::move(source);
  synchronizedService_.setIdentitySource(identitySyncSource_.get());
}

void SyncService::setUserDirectory(
    std::shared_ptr<const IUserDirectory> directory)
{
  userDirectory_ = std::move(directory);
}
