#pragma once

#include <drogon/WebSocketConnection.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct MediaSessionKey
{
  int64_t userId{0};
  std::string sessionId;
};

struct MediaSessionOpen
{
  drogon::WebSocketConnectionPtr connection;
  MediaSessionKey session;
};

class MediaSessionRegistry
{
public:
  void add(const MediaSessionOpen& open);
  void remove(const drogon::WebSocketConnectionPtr& connection);
  std::size_t closeAll(const std::string& reason);
  std::size_t closeSession(const MediaSessionKey& session);
  std::size_t closeUser(int64_t userId);
  [[nodiscard]] std::size_t size() const;

private:
  mutable std::mutex mutex_;
  std::size_t closeWhere(const std::function<bool(const MediaSessionOpen&)>& matches,
                         const std::string& reason);

  std::unordered_map<const drogon::WebSocketConnection*, MediaSessionOpen> open_;
};

namespace session_revocation
{
std::optional<MediaSessionKey> parse(const std::string& body);
std::optional<int64_t> parseUserChange(const std::string& body);
}
