#include "sync-socket.hxx"

#include <chrono>
#include <drogon/utils/coroutine.h>
#include <feature/transport/dtos/socket-frame-dto.hxx>
#include <sync/sync-errors.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

namespace
{
constexpr size_t kMaxMessageSize = 65536;

double steadySeconds()
{
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}

void SyncSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                                  std::string&& message,
                                  const drogon::WebSocketMessageType& type)
{
  if (type == drogon::WebSocketMessageType::Binary) {
    service_.handleBinary(conn, message);
    return;
  }
  if (type != drogon::WebSocketMessageType::Text)
    return;
  if (message.size() > kMaxMessageSize)
    return;

  Json::Value json = json_util::fromString(message);
  const auto frame = SocketFrameDto::fromJson(json);
  if (!frame)
    return;
  LOG_DEBUG << "SyncSocket: text " << message.substr(0, 120);

  const auto lane = service_.laneFor(conn);
  const std::string frameType = frame->type;
  const auto admission = lane->admit({.kind = FrameJobKind::Frame,
                                      .message = std::move(json),
                                      .raw = std::move(message),
                                      .type = frameType},
                                     steadySeconds());
  if (admission == FrameAdmission::Refused || admission == FrameAdmission::Stopping) {
    const ErrorDefinition& refusal = admission == FrameAdmission::Stopping
                                         ? SyncErrors::SyncStopping
                                         : SyncErrors::TooManyFrames;
    sendSocketFrameError({.conn = conn,
                          .type = frameType,
                          .status = refusal.status,
                          .error = std::string(refusal.message)});
    return;
  }
  if (admission == FrameAdmission::Start)
    service_.startDrain(conn, lane);
}

void SyncSocket::handleNewConnection(const drogon::HttpRequestPtr& req,
                                     const drogon::WebSocketConnectionPtr& conn)
{
  auto* self = this;
  drogon::async_run([self, req, conn]() -> drogon::Task<> {
    try {
      co_await self->service_.handleConnect(req, conn);
    }
    catch (const std::exception& e) {
      LOG_ERROR << "SyncSocket connect error: " << e.what();
    }
  });
}

void SyncSocket::setCameraSource(std::shared_ptr<CameraSyncSource> source)
{
  service_.setCameraSource(std::move(source));
}

void SyncSocket::setProductivitySource(
    std::shared_ptr<ProductivitySyncSource> source)
{
  service_.setProductivitySource(std::move(source));
}

void SyncSocket::setNotificationSource(
    std::shared_ptr<NotificationSyncSource> source)
{
  service_.setNotificationSource(std::move(source));
}

void SyncSocket::setIdentitySource(std::shared_ptr<IdentitySyncSource> source)
{
  service_.setIdentitySource(std::move(source));
}

void SyncSocket::setUserDirectory(
    std::shared_ptr<const IUserDirectory> directory)
{
  service_.setUserDirectory(std::move(directory));
}

void SyncSocket::setHeartbeatSource(
    std::shared_ptr<const HeartbeatSource> source)
{
  service_.setHeartbeatSource(std::move(source));
}

void SyncSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
  service_.handleDisconnect(conn);
}

void SyncSocket::setLanes(std::shared_ptr<ConnectionLanes> lanes)
{
  service_.setLanes(std::move(lanes));
}

void SyncSocket::setForwarder(std::shared_ptr<SyncForwarder> forwarder)
{
  service_.setForwarder(std::move(forwarder));
}
