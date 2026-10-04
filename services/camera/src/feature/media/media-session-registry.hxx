#pragma once

#include <drogon/WebSocketConnection.h>

#include <cstddef>
#include <cstdint>
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
  std::size_t closeSession(const MediaSessionKey& session);
  [[nodiscard]] std::size_t size() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<const drogon::WebSocketConnection*, MediaSessionOpen> open_;
};

namespace session_revocation
{
std::optional<MediaSessionKey> parse(const std::string& body);
}
