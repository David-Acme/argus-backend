#pragma once

#include <auth/user-role.hxx>

#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>

#include <atomic>
#include <chrono>
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
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
};

struct MediaIdentity
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
};

struct MediaAccessOpen
{
  drogon::WebSocketConnectionPtr connection;
  MediaCredential credential;
};

struct MediaRenewInput
{
  drogon::WebSocketConnectionPtr connection;
  std::string token;
  std::chrono::steady_clock::time_point at;
};

enum class MediaAccessVerdict : uint8_t
{
  Keep = 0,
  Expired,
  RoleChanged
};

enum class MediaRenewal : uint8_t
{
  Renewed = 0,
  Throttled,
  Unknown,
  Closed
};

class MediaAccessCheck
{
public:
  using Validate = std::function<std::optional<MediaIdentity>(const MediaCredential&)>;

  static constexpr std::chrono::seconds kMinRenewInterval{10};

  explicit MediaAccessCheck(Validate validate);

  [[nodiscard]] static Validate remote();
  [[nodiscard]] static MediaAccessVerdict judge(const MediaCredential& credential,
                                                const std::optional<MediaIdentity>& current);
  [[nodiscard]] static const char* closeReason(MediaAccessVerdict verdict);

  void add(const MediaAccessOpen& open);
  void remove(const drogon::WebSocketConnectionPtr& connection);
  drogon::Task<std::size_t> sweep();
  [[nodiscard]] std::size_t sweepNow();
  drogon::Task<MediaRenewal> renew(MediaRenewInput input);
  [[nodiscard]] MediaRenewal renewNow(const MediaRenewInput& input);
  void start(double intervalSeconds);

  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct Entry
  {
    std::weak_ptr<drogon::WebSocketConnection> connection;
    MediaCredential credential;
    std::optional<std::chrono::steady_clock::time_point> renewedAt;
  };

  static void close(const drogon::WebSocketConnectionPtr& connection, MediaAccessVerdict verdict);

  Validate validate_;
  mutable std::mutex mutex_;
  std::unordered_map<const drogon::WebSocketConnection*, Entry> open_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> sweeping_{false};
  std::atomic<int64_t> inFlight_{0};
};
