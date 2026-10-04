#include "media-session-registry.hxx"

#include <json/value.h>
#include <sync/sync-change.hxx>
#include <text/json-util.hxx>

#include <vector>

namespace
{
constexpr std::size_t kMaxSessionIdBytes = 64;
}

void MediaSessionRegistry::add(const MediaSessionOpen& open)
{
  if (!open.connection || open.session.userId <= 0 || open.session.sessionId.empty())
    return;
  std::scoped_lock lock(mutex_);
  open_[open.connection.get()] = open;
}

void MediaSessionRegistry::remove(const drogon::WebSocketConnectionPtr& connection)
{
  std::scoped_lock lock(mutex_);
  open_.erase(connection.get());
}

std::size_t MediaSessionRegistry::closeSession(const MediaSessionKey& session)
{
  std::vector<drogon::WebSocketConnectionPtr> revoked;
  {
    std::scoped_lock lock(mutex_);
    std::erase_if(open_, [&session, &revoked](const auto& entry) {
      const MediaSessionOpen& open = entry.second;
      if (open.session.userId != session.userId ||
          open.session.sessionId != session.sessionId)
        return false;
      revoked.push_back(open.connection);
      return true;
    });
  }
  for (const auto& connection : revoked)
    connection->shutdown(drogon::CloseCode::kViolation, "session_revoked");
  return revoked.size();
}

std::size_t MediaSessionRegistry::size() const
{
  std::scoped_lock lock(mutex_);
  return open_.size();
}

std::optional<MediaSessionKey> session_revocation::parse(const std::string& body)
{
  const Json::Value change = json_util::fromString(body);
  if (!change.isObject() ||
      change.get(sync_change::kActionField, "").asString() !=
          sync_change::kActionDisconnectSession)
    return std::nullopt;
  const Json::Value& user = change[sync_change::kUserField];
  const Json::Value& session = change[sync_change::kSessionField];
  if (!user.isInt64() || user.asInt64() <= 0 || !session.isString())
    return std::nullopt;
  std::string sessionId = session.asString();
  if (sessionId.empty() || sessionId.size() > kMaxSessionIdBytes)
    return std::nullopt;
  return MediaSessionKey{.userId = user.asInt64(), .sessionId = std::move(sessionId)};
}
