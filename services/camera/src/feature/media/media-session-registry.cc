#include "media-session-registry.hxx"

#include <json/value.h>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
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

std::size_t MediaSessionRegistry::closeWhere(
    const std::function<bool(const MediaSessionOpen&)>& matches, const std::string& reason)
{
  std::vector<drogon::WebSocketConnectionPtr> closed;
  {
    std::scoped_lock lock(mutex_);
    std::erase_if(open_, [&matches, &closed](const auto& entry) {
      if (!matches(entry.second))
        return false;
      closed.push_back(entry.second.connection);
      return true;
    });
  }
  for (const auto& connection : closed)
    connection->shutdown(drogon::CloseCode::kViolation, reason);
  return closed.size();
}

std::size_t MediaSessionRegistry::closeSession(const MediaSessionKey& session)
{
  return closeWhere(
      [&session](const MediaSessionOpen& open) {
        return open.session.userId == session.userId &&
               open.session.sessionId == session.sessionId;
      },
      "session_revoked");
}

std::size_t MediaSessionRegistry::closeUser(int64_t userId)
{
  return closeWhere(
      [userId](const MediaSessionOpen& open) { return open.session.userId == userId; },
      "role_changed");
}

std::size_t MediaSessionRegistry::closeAll(const std::string& reason)
{
  return closeWhere([](const MediaSessionOpen&) { return true; }, reason);
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

std::optional<int64_t> session_revocation::parseUserChange(const std::string& body)
{
  const Json::Value change = json_util::fromString(body);
  if (!change.isObject() ||
      change.get(sync_change::kKindField, "").asString() != sync_change::kKindAudit ||
      change.get("table_name", "").asString() != tableNameToString(TableName::User))
    return std::nullopt;
  const Json::Value& record = change["record_id"];
  const Json::Value& changes = change["changes"];
  if (!record.isInt64() || record.asInt64() <= 0 || !changes.isObject())
    return std::nullopt;
  if (!changes.isMember("role") && !changes.isMember("isActive"))
    return std::nullopt;
  return record.asInt64();
}
