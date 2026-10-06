#include "camera-media-socket.hxx"

#include <errors/response-exception.hxx>
#include <sync/sync-forwarder.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <auth/request-context.hxx>
#include <text/json-util.hxx>

#include <drogon/utils/coroutine.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <span>
#include <utility>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
constexpr std::string_view kAuthFrame = "camera:auth";

drogon::Task<> renewAccess(MediaAccessCheck& access, const SyncFrameInput& frame)
{
  const Json::Value& token = frame.message["payload"]["token"];
  if (!token.isString() || token.asString().empty()) {
    sendSocketFrameError({.conn = frame.conn,
                          .type = std::string(kAuthFrame),
                          .status = 400,
                          .error = "payload.token must be an access token"});
    co_return;
  }
  const MediaRenewal renewal = co_await access.renew(
      {.connection = frame.conn, .token = token.asString(), .at = std::chrono::steady_clock::now()});
  if (renewal == MediaRenewal::Renewed) {
    const auto context = frame.conn->getContext<JwtContext>();
    Json::Value ack;
    ack["type"] = "camera:auth:ok";
    ack["payload"]["role"] =
        context ? userRoleToString(context->role) : std::string();
    frame.conn->sendJson(ack);
    co_return;
  }
  if (renewal == MediaRenewal::Throttled)
    sendSocketFrameError({.conn = frame.conn,
                          .type = std::string(kAuthFrame),
                          .status = 429,
                          .error = "Renew the access at most once every 10 seconds"});
  else if (renewal == MediaRenewal::Unknown)
    sendSocketFrameError({.conn = frame.conn,
                          .type = std::string(kAuthFrame),
                          .status = 409,
                          .error = "This socket has no access to renew"});
}
}

void CameraMediaSocket::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn)
{
  if (moduleGate().disabledModuleOf(req->getPath())) {
    conn->shutdown(drogon::CloseCode::kViolation, "module_disabled");
    return;
  }
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
                              .userId = ctx.sub,
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
      if (frameType == kAuthFrame) {
        co_await renewAccess(self->access_, frame);
        co_return;
      }
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
