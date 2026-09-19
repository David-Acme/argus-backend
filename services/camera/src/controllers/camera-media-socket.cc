#include "camera-media-socket.hxx"

#include <config/app-config.hxx>
#include <feature/socket/sync/socket/sync-forwarder.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <drogon/utils/coroutine.h>

#include <memory>
#include <utility>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
} // namespace

void CameraMediaSocket::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  conn->setContext(std::make_shared<JwtContext>(ctx));
}

void CameraMediaSocket::handleNewMessage(
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
    try {
      const bool handled = co_await self->service_.handleText(
          {.conn = conn, .message = json, .raw = raw});
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
  service_.handleClose(conn);
}
