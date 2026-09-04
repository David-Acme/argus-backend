#include "sync-relay.hxx"

#include <config/app-config.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

struct LegacySyncRelay::Session
{
  std::string token;
  std::string userAgent;
  std::string forwardedFor;
  drogon::WebSocketClientPtr client;
  drogon::WebSocketConnectionPtr legacy;
  std::vector<Frame> pending;
  bool connecting{false};
  bool failed{false};
  bool closing{false};
};

LegacySyncConfig LegacySyncConfig::resolve()
{
  LegacySyncConfig config;
  config.syncUrl = ConfigService::getString("legacy.sync_url");
  config.dbPath = ConfigService::getString("legacy.db");
  if (config.dbPath.empty())
    config.dbPath = "database/argus.db";
  return config;
}

bool relayAllowedText(std::string_view type)
{
  return type.rfind("camera:", 0) == 0 || type.rfind("voice:", 0) == 0;
}

LegacySyncRelay::LegacySyncRelay(LegacySyncConfig config)
    : config_(std::move(config))
{
}

void LegacySyncRelay::onConnect(const drogon::HttpRequestPtr& req,
                                const drogon::WebSocketConnectionPtr& conn)
{
  auto session = std::make_shared<Session>();
  session->token = JwtFilter::extractToken(req);
  session->userAgent = req->getHeader("User-Agent");
  session->forwardedFor = req->getHeader("X-Forwarded-For");

  std::lock_guard<std::mutex> lock(sessionsMutex_);
  sessions_[conn.get()] = std::move(session);
  (void)conn;
}

std::shared_ptr<LegacySyncRelay::Session>
LegacySyncRelay::sessionFor(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  if (it == sessions_.end()) {
    it = sessions_
             .emplace(conn.get(), std::make_shared<Session>())
             .first;
  }
  return it->second;
}

std::shared_ptr<LegacySyncRelay::Session>
LegacySyncRelay::takeSession(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  if (it == sessions_.end())
    return nullptr;
  auto session = std::move(it->second);
  sessions_.erase(it);
  return session;
}

drogon::Task<bool> LegacySyncRelay::forwardText(
    const drogon::WebSocketConnectionPtr& conn, const Json::Value& message,
    std::string_view raw)
{
  (void)message;
  if (config_.syncUrl.empty())
    throw ResponseException("Legacy sync relay is not configured", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  auto session = sessionFor(conn);
  if (session->failed)
    throw ResponseException("Legacy sync unavailable", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
  if (session->closing)
    co_return true;

  if (session->legacy) {
    session->legacy->send(raw.data(), raw.size(),
                          drogon::WebSocketMessageType::Text);
    co_return true;
  }

  if (session->pending.size() >= 256) {
    session->failed = true;
    throw ResponseException("Legacy sync queue overflow", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
  }
  session->pending.push_back(Frame{std::string(raw), false});
  if (!session->connecting) {
    session->connecting = true;
    drogon::async_run([this, conn, session]() -> drogon::Task<void> {
      co_await openSession(conn, session);
    });
  }
  co_return true;
}

void LegacySyncRelay::forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                                    const std::string& data)
{
  if (config_.syncUrl.empty())
    return;

  auto session = sessionFor(conn);
  if (session->failed || session->closing)
    return;
  if (session->legacy) {
    session->legacy->send(data.data(), data.size(),
                          drogon::WebSocketMessageType::Binary);
    return;
  }
  session->pending.push_back(Frame{data, true});
}

void LegacySyncRelay::onClose(const drogon::WebSocketConnectionPtr& conn)
{
  auto session = takeSession(conn);
  if (!session)
    return;
  session->closing = true;
  if (session->client)
    session->client->stop();
  session->legacy.reset();
}

drogon::Task<void>
LegacySyncRelay::openSession(const drogon::WebSocketConnectionPtr& conn,
                             std::shared_ptr<Session> session)
{
  // The client connection's loop: every session callback stays on it.
  auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  if (!loop)
    loop = drogon::app().getIOLoop(0);

  if (!config_.syncUrl.empty() && session->token.empty()) {
    LOG_WARN << "Sync relay: refusing legacy connection without a token";
    session->failed = true;
    conn->shutdown(drogon::CloseCode::kNormalClosure);
    co_return;
  }

  auto client = drogon::WebSocketClient::newWebSocketClient(config_.syncUrl,
                                                            loop, false, false);
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
        if (!relayAllowedText(json["type"].asString()))
          return;
        conn->send(message.data(), message.size(),
                   drogon::WebSocketMessageType::Text);
      });

  client->setConnectionClosedHandler(
      [conn, session](const drogon::WebSocketClientPtr&) {
        session->legacy.reset();
        if (session->closing || conn->disconnected())
          return;
        session->failed = true;
        conn->shutdown(drogon::CloseCode::kNormalClosure);
      });

  const auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath("/sync");
  req->addHeader("Authorization", "Bearer " + session->token);
  if (!session->userAgent.empty())
    req->addHeader("User-Agent", session->userAgent);
  if (!session->forwardedFor.empty())
    req->addHeader("X-Forwarded-For", session->forwardedFor);

  try {
    const auto resp = co_await client->connectToServerCoro(req);
    (void)resp;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Sync relay: legacy connection failed: " << e.what();
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

  session->legacy = client->getConnection();
  for (const auto& frame : session->pending) {
    session->legacy->send(frame.data.data(), frame.data.size(),
                          frame.binary ? drogon::WebSocketMessageType::Binary
                                       : drogon::WebSocketMessageType::Text);
  }
  session->pending.clear();
}