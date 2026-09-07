#pragma once

#include <core/push-queue.hxx>
#include <core/tunnel-mux.hxx>
#include <net/tcp-listener.hxx>
#include <net/tcp-peer.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace tunnel
{
struct RelayOptions
{
  std::string host{"0.0.0.0"};
  uint16_t devicePort{7100};
  uint16_t homePort{7101};
  std::string secret;
  TunnelMux::Limits limits;
  // [push] enabled + queue capacity (F5-5).
  bool pushEnabled{false};
  size_t pushQueueCapacity{256};
};

// US-side relay: accepts device connections and multiplexes each one over
// the single authenticated home link; single-tenant, ephemeral registry.
class TunnelRelay : public MuxDelegate
{
public:
  TunnelRelay(PollLoop& loop, RelayOptions options);
  ~TunnelRelay() override;

  // Returns false when a listener bind fails.
  bool start();
  void stop();

  // Force-closes the home link (ops/tests).
  void dropLink() { mux_.dropLink(); }

  bool hasHome() const { return mux_.homeActive(); }
  uint16_t devicePort() const { return devicePort_; }
  uint16_t homePort() const { return homePort_; }
  size_t streamCount() const { return mux_.streamCount(); }
  size_t pendingBytes() const { return mux_.pendingBytes(); }

  // Push-intent ingress (F5-5): thread-safe post into the loop thread; the
  // queue survives a home-link drop and drains once the link re-authenticates.
  void postPushIntent(std::string payload);
  size_t pushQueued() const { return pushQueue_.size(); }
  uint64_t pushReceived() const { return pushQueue_.received(); }
  uint64_t pushDropped() const { return pushQueue_.dropped(); }
  uint64_t pushForwarded() const
  {
    return pushForwarded_.load(std::memory_order_relaxed);
  }

private:
  // MuxDelegate
  const std::string& authSecret() const override { return options_.secret; }
  bool validatesAuth() const override { return true; }
  void onAuthAccepted() override;
  void onLinkUp() override;
  void onLinkDown() override;

  void onPushIntent(const std::string& payload);
  void drainPushQueue();

  void onHomeAccepted(int fd, const std::string& peerIp, uint16_t peerPort);
  void onDeviceAccepted(int fd, const std::string& peerIp, uint16_t peerPort);
  void scheduleSweep();

  PollLoop& loop_;
  RelayOptions options_;
  TunnelMux mux_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::weak_ptr<bool> aliveToken_{alive_};
  std::unique_ptr<TcpListener> homeListener_;
  std::unique_ptr<TcpListener> deviceListener_;
  uint16_t devicePort_{0};
  uint16_t homePort_{0};
  std::atomic<bool> stopped_{false};
  PushQueue pushQueue_{options_.pushQueueCapacity};
  std::atomic<uint64_t> pushForwarded_{0};
};
} // namespace tunnel
