#pragma once

#include <core/tunnel-mux.hxx>
#include <net/tcp-listener.hxx>
#include <net/tcp-peer.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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

class TunnelRelay : public MuxDelegate
{
public:
  TunnelRelay(PollLoop& loop, RelayOptions options);
  ~TunnelRelay() override;

  bool start();
  void stop();

  void dropLink() { mux_.dropLink(); }

  bool hasHome() const { return mux_.homeActive(); }
  uint16_t devicePort() const { return devicePort_; }
  uint16_t homePort() const { return homePort_; }
  size_t streamCount() const { return mux_.streamCount(); }
  size_t pendingHomeCount() const { return mux_.pendingHomeCount(); }
  size_t pendingBytes() const { return mux_.pendingBytes(); }

private:
  const std::string& authSecret() const override { return options_.secret; }
  bool validatesAuth() const override { return true; }
  void onLinkUp() override;
  void onLinkDown() override;
  void onStreamClosed(uint32_t streamId) override;

  struct PeerAcceptedInput
  {
    int fd{-1};
    const std::string& peerIp;
    uint16_t peerPort{0};
  };

  void onHomeAccepted(const PeerAcceptedInput& input);
  void onDeviceAccepted(const PeerAcceptedInput& input);
  void scheduleSweep();

  PollLoop& loop_;
  RelayOptions options_;
  TunnelMux mux_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::weak_ptr<bool> aliveToken_{alive_};
  std::shared_ptr<TcpListener> homeListener_;
  std::shared_ptr<TcpListener> deviceListener_;
  uint16_t devicePort_{0};
  uint16_t homePort_{0};
  std::atomic<bool> stopped_{false};
  std::unordered_map<uint32_t, std::string> streamIps_;
  std::unordered_map<std::string, int> ipStreams_;
};
}
