#pragma once

#include <auth/user-role.hxx>

#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct MediaCredential
{
  std::string token;
  std::string deviceHash;
  std::string origin;
  UserRole role{UserRole::Guest};
};

struct MediaAccessOpen
{
  drogon::WebSocketConnectionPtr connection;
  MediaCredential credential;
};

enum class MediaAccessVerdict : uint8_t
{
  Keep = 0,
  Expired,
  RoleChanged
};

class MediaAccessCheck
{
public:
  using Validate = std::function<std::optional<UserRole>(const MediaCredential&)>;

  explicit MediaAccessCheck(Validate validate);

  [[nodiscard]] static Validate remote();
  [[nodiscard]] static MediaAccessVerdict judge(const MediaCredential& credential,
                                                const std::optional<UserRole>& current);

  void add(const MediaAccessOpen& open);
  void remove(const drogon::WebSocketConnectionPtr& connection);
  drogon::Task<std::size_t> sweep();
  void start(double intervalSeconds);

  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct Entry
  {
    std::weak_ptr<drogon::WebSocketConnection> connection;
    MediaCredential credential;
  };

  Validate validate_;
  mutable std::mutex mutex_;
  std::unordered_map<const drogon::WebSocketConnection*, Entry> open_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> sweeping_{false};
  std::atomic<int64_t> inFlight_{0};
};
