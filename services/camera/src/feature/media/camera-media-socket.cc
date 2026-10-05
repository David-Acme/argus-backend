#include "camera-media-socket.hxx"

#include <errors/response-exception.hxx>
#include <sync/sync-forwarder.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <text/json-util.hxx>

#include <drogon/utils/coroutine.h>

#include <memory>
#include <span>
#include <utility>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
}

void CameraMediaSocket::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  conn->setContext(std::make_shared<JwtContext>(ctx));
  sessions_.add({.connection = conn,
                 .session = {.userId = ctx.sub, .sessionId = ctx.sessionId}});
  const auto& device = req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  access_.add({.connection = conn,
               .credential = {.token = JwtFilter::extractToken(req),
                              .deviceHash = ctx.deviceHash,
                              .origin = device.origin == SessionOrigin::Unknown
                                            ? std::string{}
                                            : sessionOriginToString(device.origin),
                              .role = ctx.role}});
}

void CameraMediaSocket::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&& message,
    const drogon::WebSocketMessageType& type)
{
  if (type == drogon::WebSocketMessageType::Binary) {
    talk_.handleBinary(conn, std::span(reinterpret_cast<const uint8_t*>(message.data()),
                                       message.size()));
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
  if (!json.isObject() || !json["type"].isString())
    return;

  auto* self = this;
  drogon::async_run([self, conn, json = std::move(json),
                     raw = std::move(message)]() mutable -> drogon::Task<> {
    const std::string frameType = json.get("type", "").asString();
    try {
      const SyncFrameInput frame{.conn = conn, .message = json, .raw = raw};
      const bool handled = CameraTalkService::handles(frameType)
                               ? co_await self->talk_.handleText(frame)
                               : co_await self->service_.handleText(frame);
      if (!handled)
        sendSocketFrameError({.conn = conn,
                              .type = frameType,
                              .status = 400,
                              .error = "Unknown message type"});
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

void CameraMediaSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
  sessions_.remove(conn);
  access_.remove(conn);
  talk_.handleClose(conn);
  service_.handleClose(conn);
}
