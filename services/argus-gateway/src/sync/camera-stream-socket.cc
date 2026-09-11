#include "camera-stream-socket.hxx"

#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <drogon/utils/coroutine.h>

#include <utility>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
} // namespace

void CameraStreamSocket::setRelay(std::shared_ptr<CameraStreamRelay> relay)
{
  relay_ = std::move(relay);
}

void CameraStreamSocket::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn)
{
  if (relay_)
    relay_->onConnect(req, conn);
}

void CameraStreamSocket::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&& message,
    const drogon::WebSocketMessageType& type)
{
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
  if (!json.isObject() || !json["type"].isString())
    return;

  auto* self = this;
  drogon::async_run([self, conn, json = std::move(json),
                     raw = std::move(message)]() mutable -> drogon::Task<> {
    const std::string frameType = json.get("type", "").asString();
    if (!self->relay_) {
      sendSocketFrameError({.conn = conn,
                            .type = frameType,
                            .status = 503,
                            .error = "Camera stream is not configured"});
      co_return;
    }
    try {
      const bool handled = co_await self->relay_->forwardText(
          {.conn = conn, .message = json, .raw = raw});
      if (!handled) {
        sendSocketFrameError({.conn = conn,
                              .type = frameType,
                              .status = 400,
                              .error = "Unknown message type"});
      }
    }
    catch (const ResponseException& ex) {
      sendSocketFrameError({.conn = conn,
                            .type = frameType,
                            .status = ex.statusCode(),
                            .error = ex.what()});
    }
    catch (const std::exception& ex) {
      sendSocketFrameError(
          {.conn = conn, .type = frameType, .status = 500, .error = ex.what()});
    }
  });
}

void CameraStreamSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
  if (relay_)
    relay_->onClose(conn);
}
