#pragma once

#include <feature/heartbeat/infra/presence-directory.hxx>
#include <feature/heartbeat/services/heartbeat-service.hxx>
#include <feature/heartbeat/services/presence-board.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <vector>

class NatsBus;

class HeartbeatFeed
{
public:
  using Emit = std::function<void(int64_t userId, std::string_view frame)>;

  struct Dependencies
  {
    NatsBus* bus{nullptr};
    std::shared_ptr<PresenceBoard> board;
    std::shared_ptr<const HeartbeatService> heartbeat;
    std::shared_ptr<const PresenceDirectory> directory;
    Emit emit;
  };

  struct Config
  {
    std::string presenceSubject;
    std::string guardHeartbeatSubject;
    double refillSeconds{300};
  };

  HeartbeatFeed(Dependencies dependencies, Config config);
  ~HeartbeatFeed();

  HeartbeatFeed(const HeartbeatFeed&) = delete;
  HeartbeatFeed& operator=(const HeartbeatFeed&) = delete;

  void start();
  void stop();
  void requestStop();
  [[nodiscard]] bool drained() const;

  bool ingestPresence(std::string_view payload);
  void ingestGuardHeartbeat(int64_t at);
  bool refill();

private:
  void publishTo(int64_t userId) const;

  Dependencies dependencies_;
  Config config_;
  std::vector<uint64_t> subscriptions_;
  std::optional<trantor::TimerId> refillTimer_;
  std::shared_ptr<bool> alive_;
  std::shared_ptr<std::atomic<int>> refilling_{
      std::make_shared<std::atomic<int>>(0)};
};

namespace heartbeat
{
[[nodiscard]] std::optional<PresenceEntry> parsePresence(std::string_view payload);

}
