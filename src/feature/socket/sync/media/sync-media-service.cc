#include "sync-media-service.hxx"

#include <config/app-config.hxx>
#include <shared/access/role-access.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <filter/jwt/jwt-filter.hxx>

SyncMediaService::SyncMediaService()
{
  if (const int v = ConfigService::getInt("streaming.hub_max_subs_per_client");
      v > 0)
    maxSubsPerClient_ = v;
}

std::shared_ptr<DrogonStreamSink>
SyncMediaService::sinkFor(const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  const auto it = sinks_.find(conn.get());
  return it == sinks_.end() ? nullptr : it->second;
}

void SyncMediaService::storeSink(const drogon::WebSocketConnectionPtr& conn,
                                 const std::shared_ptr<DrogonStreamSink>& sink) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  sinks_[conn.get()] = sink;
}

void SyncMediaService::dropSink(const drogon::WebSocketConnectionPtr& conn) const
{
  std::lock_guard<std::mutex> lock(sinksMutex_);
  sinks_.erase(conn.get());
}

drogon::Task<bool> SyncMediaService::forwardText(
    const drogon::WebSocketConnectionPtr& conn, const Json::Value& message,
    std::string_view raw)
{
  (void)raw;
  const std::string type = message["type"].asString();
  const Json::Value& payload = message["payload"];
  const auto& ctx = conn->getContextRef<JwtContext>();

  if (type == "camera:subscribe") {
    if (!role_access::hasAccess(ctx.role, TableName::Camera,
                                RolePermission::Read))
      throw ResponseException("Forbidden", 403,
                              AppConfig::ERROR_CODE_FORBIDDEN);

    const int64_t cameraId = payload.get("cameraId", 0).asInt64();
    if (cameraId <= 0)
      throw ResponseException("Invalid cameraId", 400,
                              AppConfig::ERROR_CODE_BAD_REQUEST);
    const std::string quality = payload.get("quality", "main").asString();

    auto sink = sinkFor(conn);
    if (!sink) {
      int64_t window = 128 * 1024;
      if (const int64_t v = ConfigService::getInt("streaming.hub_window_bytes");
          v > 0)
        window = v;
      sink = std::make_shared<DrogonStreamSink>(conn, window);
      storeSink(conn, sink);
    }
    if (sink->subscriptions() >= maxSubsPerClient_)
      throw ResponseException("Too many camera subscriptions", 429,
                              AppConfig::ERROR_CODE_TOO_MANY_REQUESTS);

    StreamHub::SubscribeInput input;
    input.sink = sink;
    input.cameraId = cameraId;
    input.quality = quality;
    std::string error;
    const uint16_t subId = StreamHub::instance().subscribe(input, error);
    if (subId == 0)
      throw ResponseException(error.empty() ? "subscribe_failed" : error, 503,
                              AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
    sink->addSubscription();

    Json::Value resp;
    resp["subId"] = subId;
    resp["mime"] = "video/mp4";
    Json::Value out;
    out["type"] = "camera:ready";
    out["payload"] = resp;
    conn->sendJson(out);
    co_return true;
  }

  if (type == "camera:ack") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    const int64_t bytes = payload.get("bytes", 0).asInt64();
    StreamHub::instance().ack(subId, bytes);
    co_return true;
  }

  if (type == "camera:unsubscribe") {
    const uint16_t subId =
        static_cast<uint16_t>(payload.get("subId", 0).asUInt());
    StreamHub::instance().unsubscribe(subId);
    if (auto sink = sinkFor(conn))
      sink->dropSubscription();
    co_return true;
  }

  if (type == "voice:start") {
    // Language comes from the registered user (`user.lang`); missing or
    // invalid falls back to the system default inside the voice service.
    VoiceLang lang = VoiceLang::System;
    std::string name;
    if (ctx.sub > 0) {
      UserRepository userRepository;
      const auto user = co_await userRepository.findById(ctx.sub);
      if (user) {
        lang = voiceLangFromString(user->lang);
        name = user->name;
      }
    }
    voiceSessionService_.start(conn, ctx.sub, lang, name);
    co_return true;
  }

  if (type == "voice:stop") {
    voiceSessionService_.stop(conn);
    co_return true;
  }

  if (type == "voice:skip") {
    voiceSessionService_.skip(conn);
    co_return true;
  }

  co_return false;
}

void SyncMediaService::forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                                     const std::string& data)
{
  voiceSessionService_.feedPcm(conn, data.data(), data.size());
}

void SyncMediaService::onClose(const drogon::WebSocketConnectionPtr& conn)
{
  if (auto sink = sinkFor(conn))
    StreamHub::instance().closeAll(sink.get());
  dropSink(conn);
  voiceSessionService_.stop(conn);
}
