#include "sync-socket.hxx"

#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/transport/dtos/socket-frame-dto.hxx>
#include <sync/sync-errors.hxx>
#include <text/json-util.hxx>
#include <validation/validator.hxx>
#include <trantor/utils/Logger.h>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
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

  Json::Value json;
  try {
    json = json_util::fromString(message);
  }
  catch (...) {
    return;
  }
  const auto frame = SocketFrameDto::fromJson(json);
  if (!frame)
    return;
  LOG_DEBUG << "SyncSocket: text " << message.substr(0, 120);

  auto* self = this;
  drogon::async_run([self, conn, json = std::move(json), type = frame->type,
                     raw = std::move(message)]() mutable
                    -> drogon::Task<> {
    try {
      co_await self->service_.handleMessage(
          {.conn = conn, .message = json, .raw = raw});
    }
    catch (const ValidationException& ex) {
      sendSocketFrameError({.conn = conn,
                            .type = type,
                            .status = 422,
                            .error = ex.what()});
    }
    catch (const ResponseException& ex) {
      sendSocketFrameError({.conn = conn,
                            .type = type,
                            .status = ex.statusCode(),
                            .error = ex.what()});
    }
    catch (const std::exception& ex) {
      LOG_ERROR << "SyncSocket: " << type << " failed: " << ex.what();
      sendSocketFrameError({.conn = conn,
                            .type = type,
                            .status = SyncErrors::FrameFailed.status,
                            .error = std::string(SyncErrors::FrameFailed.message)});
    }
  });
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

void SyncSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
  service_.handleDisconnect(conn);
}

void SyncSocket::setForwarder(std::shared_ptr<SyncForwarder> forwarder)
{
  service_.setForwarder(std::move(forwarder));
}
