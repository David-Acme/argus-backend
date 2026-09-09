#pragma once

#include <core/push-queue.hxx>
#include <core/tunnel-mux.hxx>
#include <net/tcp-peer.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace tunnel
{
struct ClientOptions
{
  std::string relayHost{"127.0.0.1"};
  uint16_t relayPort{7101};
  std::string gatewayHost{"127.0.0.1"};
  uint16_t gatewayPort{7024};
  std::string secret;
  int reconnectWaitMs{2000};
  int maxReconnects{60};
  int pingIntervalSeconds{30};
  TunnelMux::Limits limits;
  size_t pushQueueCapacity{256};
};

// Home-side client: one persistent link to the relay, each stream dialed fresh on the gateway listener.
class TunnelClient : public MuxDelegate
{
public:
  TunnelClient(PollLoop& loop, ClientOptions options);
  ~TunnelClient() override;

  void start();
  void stop();

  // Force-closes the home link (ops/tests); triggers the reconnect loop.
  void dropLink() { mux_.dropLink(); }

  bool homeConnected() const { return homeConnected_.load(); }
  bool homeActive() const { return mux_.homeActive(); }
  size_t streamCount() const { return mux_.streamCount(); }
  int reconnectAttempts() const { return reconnectAttempts_.load(); }
  bool gaveUp() const { return gaveUp_.load(); }
  size_t pendingBytes() const { return mux_.pendingBytes(); }

  // Push-intent queue counters for /health.
  size_t pushQueued() const { return pushQueue_.size(); }
  uint64_t pushReceived() const { return pushQueue_.received(); }
  uint64_t pushDropped() const { return pushQueue_.dropped(); }

private:
  // MuxDelegate
  const std::string& authSecret() const override { return options_.secret; }
  bool validatesAuth() const override { return false; }
  void onAuthAccepted() override;
  void onAuthRejected() override;
  void onRemoteOpen(uint32_t streamId) override;
  void onPushFrame(const std::string& payload) override;
  void onLinkUp() override;
  void onLinkDown() override;

  void connectHome();
  void closePending();
  void scheduleReconnect();
  void scheduleSweep();
  void schedulePing();

  PollLoop& loop_;
  ClientOptions options_;
  TunnelMux mux_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::weak_ptr<bool> aliveToken_{alive_};
  TcpPeer::Ptr pendingHome_;
  std::unordered_map<uint32_t, TcpPeer::Ptr> pendingDials_;
  std::atomic<bool> homeConnected_{false};
  std::atomic<int> reconnectAttempts_{0};
  std::atomic<bool> gaveUp_{false};
  std::atomic<bool> stopped_{false};
  PushQueue pushQueue_{options_.pushQueueCapacity};
};
} // namespace tunnel
