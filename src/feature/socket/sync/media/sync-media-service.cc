#include "sync-media-service.hxx"

#include <filter/jwt/jwt-filter.hxx>
#include <shared/repositories/user/user-repository.hxx>

drogon::Task<bool> SyncMediaService::forwardText(
    const drogon::WebSocketConnectionPtr& conn, const Json::Value& message,
    std::string_view raw)
{
  (void)raw;
  const std::string type = message["type"].asString();

  if (type == "voice:start") {
    // Language comes from the registered user (`user.lang`); missing or
    // invalid falls back to the system default inside the voice service.
    const auto& ctx = conn->getContextRef<JwtContext>();
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
  voiceSessionService_.stop(conn);
}
