#include "sync-socket.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/exceptions/response-exception.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/validation/validator.hxx>
#include <trantor/utils/Logger.h>

namespace
{
constexpr size_t kMaxMessageSize = 65536;
} // namespace

void SyncSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
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

  auto* self = this;
  drogon::async_run([self, conn, json = std::move(json)]() mutable
                    -> drogon::Task<> {
    try {
      co_await self->service_.handleMessage(conn, json);
    }
    catch (const ValidationException& ex) {
      Json::Value errResp;
      errResp["type"] = json.get("type", "").asString() + "_error";
      errResp["status"] = 422;
      errResp["error"] = ex.what();
      conn->sendJson(errResp);
    }
    catch (const ResponseException& ex) {
      Json::Value errResp;
      errResp["type"] = json.get("type", "").asString() + "_error";
      errResp["status"] = ex.statusCode();
      errResp["error"] = ex.what();
      conn->sendJson(errResp);
    }
    catch (const std::exception& ex) {
      Json::Value errResp;
      errResp["type"] = json.get("type", "").asString() + "_error";
      errResp["status"] = 500;
      errResp["error"] = ex.what();
      conn->sendJson(errResp);
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

void SyncSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
  service_.handleDisconnect(conn);
}
