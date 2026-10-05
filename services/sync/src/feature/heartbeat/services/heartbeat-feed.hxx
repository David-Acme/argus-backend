#pragma once

#include <feature/heartbeat/infra/presence-directory.hxx>
#include <feature/heartbeat/services/heartbeat-service.hxx>
#include <feature/heartbeat/services/presence-board.hxx>

#include <cstdint>
#include <functional>
#include <memory>
#include <nats/push-intent-sink.hxx>
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
    const push_intent::PushIntentSink* push{nullptr};
    Emit emit;
  };

  struct Config
  {
    std::string presenceSubject;
    std::string guardHeartbeatSubject;
    double pushIntervalSeconds{900};
    double refillSeconds{300};
  };

  HeartbeatFeed(Dependencies dependencies, Config config);
  ~HeartbeatFeed();

  HeartbeatFeed(const HeartbeatFeed&) = delete;
  HeartbeatFeed& operator=(const HeartbeatFeed&) = delete;

  void start();
  void stop();

  bool ingestPresence(std::string_view payload);
  void ingestGuardHeartbeat(int64_t at);
  [[nodiscard]] size_t publishPushHeartbeats() const;
  bool refill();

private:
  void publishTo(int64_t userId) const;

  Dependencies dependencies_;
  Config config_;
  std::vector<uint64_t> subscriptions_;
  std::optional<trantor::TimerId> pushTimer_;
  std::optional<trantor::TimerId> refillTimer_;
  std::shared_ptr<bool> alive_;
};

namespace heartbeat
{
[[nodiscard]] std::optional<PresenceEntry> parsePresence(std::string_view payload);

[[nodiscard]] PushIntent pushIntentFor(int64_t userId, const Json::Value& heartbeat);
}
