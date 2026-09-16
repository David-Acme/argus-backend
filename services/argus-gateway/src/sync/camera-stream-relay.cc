#include "camera-stream-relay.hxx"

#include <config/app-config.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>
#include <vector>

namespace
{
constexpr std::string_view kCameraPrefix = "camera:";
constexpr std::string_view kMediaPath = "/media";
// Pending frames are bounded; a stalled upstream leg must not grow unbounded.
constexpr size_t kPendingLimit = 256;
} // namespace

struct CameraStreamRelay::Session
{
  std::string token;
  std::string userAgent;
  std::string forwardedFor;
  drogon::WebSocketClientPtr client;
  drogon::WebSocketConnectionPtr leg;
  std::vector<std::string> pending;
  bool connecting{false};
  bool failed{false};
  bool closing{false};
};

bool isCameraStreamFrame(std::string_view type)
{
  return type.rfind(kCameraPrefix, 0) == 0;
}

CameraStreamConfig CameraStreamConfig::resolve()
{
  CameraStreamConfig config;
  config.streamUrl = ConfigService::getString("camera.stream_url");
  return config;
}

CameraStreamRelay::CameraStreamRelay(std::string streamUrl)
    : streamUrl_(std::move(streamUrl))
{
}

void CameraStreamRelay::onConnect(const drogon::HttpRequestPtr& req,
                                  const drogon::WebSocketConnectionPtr& conn)
{
  auto session = std::make_shared<Session>();
  session->token = JwtFilter::extractToken(req);
  session->userAgent = req->getHeader("User-Agent");
  // The gateway is the only peer that saw the client, so the device hash uses
  // the peer IP and never a client-supplied X-Forwarded-For.
  session->forwardedFor = conn->peerAddr().toIp();

  std::lock_guard<std::mutex> lock(sessionsMutex_);
  sessions_[conn.get()] = std::move(session);
}

std::shared_ptr<CameraStreamRelay::Session>
CameraStreamRelay::sessionFor(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  if (it == sessions_.end())
    it = sessions_.emplace(conn.get(), std::make_shared<Session>()).first;
  return it->second;
}

std::shared_ptr<CameraStreamRelay::Session>
CameraStreamRelay::takeSession(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  if (it == sessions_.end())
    return nullptr;
  auto session = std::move(it->second);
  sessions_.erase(it);
  return session;
}

drogon::Task<bool> CameraStreamRelay::forwardText(const SyncFrameInput& input)
{
  if (!isCameraStreamFrame(input.message["type"].asString()))
    co_return false;

  const drogon::WebSocketConnectionPtr& conn = input.conn;
  const std::string_view raw = input.raw;

  auto session = sessionFor(conn);
  if (session->failed)
    throw ResponseException(
        {.message = "Camera stream unavailable",
         .statusCode = 503,
         .errorCode = AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
  if (session->closing)
    co_return true;

  if (session->leg) {
    session->leg->send(raw.data(), raw.size(),
                       drogon::WebSocketMessageType::Text);
    co_return true;
  }

  if (session->pending.size() >= kPendingLimit) {
    session->failed = true;
    throw ResponseException(
        {.message = "Camera stream queue overflow",
         .statusCode = 503,
         .errorCode = AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
  }
  session->pending.push_back(std::string(raw));
  if (!session->connecting) {
    session->connecting = true;
    drogon::async_run([this, conn, session]() -> drogon::Task<void> {
      try {
        co_await openSession(conn, session);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera stream relay: session open failed: "
                 << error.what();
      }
      catch (...) {
        LOG_WARN << "Camera stream relay: session open failed with unknown "
                    "error";
      }
      co_return;
    });
  }
  co_return true;
}

void CameraStreamRelay::onClose(const drogon::WebSocketConnectionPtr& conn)
{
  auto session = takeSession(conn);
  if (!session)
    return;
  session->closing = true;
  if (session->client)
    session->client->stop();
  session->leg.reset();
}

drogon::Task<void>
CameraStreamRelay::openSession(const drogon::WebSocketConnectionPtr& conn,
                               std::shared_ptr<Session> session)
{
  // The client connection's loop: every session callback stays on it.
  auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  if (!loop)
    loop = drogon::app().getIOLoop(0);

  if (session->token.empty()) {
    LOG_WARN << "Camera stream relay: refusing connection without a token";
    session->failed = true;
    conn->shutdown(drogon::CloseCode::kNormalClosure);
    co_return;
  }

  auto client = drogon::WebSocketClient::newWebSocketClient(streamUrl_, loop,
                                                            false, false);
  session->client = client;
  client->setMessageHandler(
      [conn, session](std::string&& message, const drogon::WebSocketClientPtr&,
                      const drogon::WebSocketMessageType& type) {
        if (session->closing || conn->disconnected())
          return;
        if (type == drogon::WebSocketMessageType::Binary) {
          conn->send(message.data(), message.size(),
                     drogon::WebSocketMessageType::Binary);
          return;
        }
        if (type != drogon::WebSocketMessageType::Text)
          return;
        const Json::Value json = json_util::fromString(message);
        if (!json.isObject() || !json["type"].isString())
          return;
        if (!isCameraStreamFrame(json["type"].asString()))
          return;
        conn->send(message.data(), message.size(),
                   drogon::WebSocketMessageType::Text);
      });

  client->setConnectionClosedHandler(
      [conn, session](const drogon::WebSocketClientPtr&) {
        session->leg.reset();
        if (session->closing || conn->disconnected())
          return;
        session->failed = true;
        conn->shutdown(drogon::CloseCode::kNormalClosure);
      });

  const auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath(std::string(kMediaPath));
  req->addHeader("Authorization", "Bearer " + session->token);
  if (!session->userAgent.empty())
    req->addHeader("User-Agent", session->userAgent);
  req->addHeader("X-Forwarded-For", session->forwardedFor);

  try {
    const auto resp = co_await client->connectToServerCoro(req);
    (void)resp;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Camera stream relay: leg connection failed: " << e.what();
    session->client.reset();
    session->failed = true;
    if (!session->closing && !conn->disconnected())
      conn->shutdown(drogon::CloseCode::kNormalClosure);
    co_return;
  }

  if (session->closing || conn->disconnected()) {
    client->stop();
    co_return;
  }

  session->leg = client->getConnection();
  for (const auto& frame : session->pending)
    session->leg->send(frame.data(), frame.size(),
                       drogon::WebSocketMessageType::Text);
  session->pending.clear();
}
