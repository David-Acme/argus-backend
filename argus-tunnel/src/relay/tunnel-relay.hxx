#pragma once

#include <core/tunnel-mux.hxx>
#include <net/tcp-listener.hxx>
#include <net/tcp-peer.hxx>

#include <atomic>
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

private:
  // MuxDelegate
  const std::string& authSecret() const override { return options_.secret; }
  bool validatesAuth() const override { return true; }
  void onLinkUp() override;
  void onLinkDown() override;

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
};
} // namespace tunnel
