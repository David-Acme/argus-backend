#include "tunnel-relay.hxx"

#include <drogon/drogon.h>

#include <unistd.h>

#include <utility>

namespace tunnel
{
TunnelRelay::TunnelRelay(PollLoop& loop, RelayOptions options)
    : loop_(loop), options_(std::move(options)),
      mux_({.loop = loop, .limits = options_.limits, .delegate = this})
{
}

TunnelRelay::~TunnelRelay()
{
  stop();
}

bool TunnelRelay::start()
{
  TcpListener::Params homeParams;
  homeParams.loop = &loop_;
  homeParams.ip = options_.host;
  homeParams.port = options_.homePort;
  homeParams.onAccept = [this](int fd, const std::string& ip, uint16_t port) {
    onHomeAccepted({.fd = fd, .peerIp = ip, .peerPort = port});
  };
  homeListener_ = TcpListener::create(homeParams);
  if (!homeListener_)
    return false;
  homePort_ = homeListener_->boundPort();

  TcpListener::Params deviceParams;
  deviceParams.loop = &loop_;
  deviceParams.ip = options_.host;
  deviceParams.port = options_.devicePort;
  deviceParams.onAccept = [this](int fd, const std::string& ip, uint16_t port) {
    onDeviceAccepted({.fd = fd, .peerIp = ip, .peerPort = port});
  };
  deviceListener_ = TcpListener::create(deviceParams);
  if (!deviceListener_)
    return false;
  devicePort_ = deviceListener_->boundPort();

  loop_.post([this] { scheduleSweep(); });
  LOG_INFO << "argus-relay: home listener on " << options_.host << ":"
           << homePort_ << ", device listener on " << options_.host << ":"
           << devicePort_;
  return true;
}

void TunnelRelay::stop()
{
  if (stopped_.exchange(true))
    return;
  loop_.post([this] {
    homeListener_.reset();
    deviceListener_.reset();
    mux_.dropLink();
  });
}

void TunnelRelay::onLinkUp()
{
  LOG_INFO << "argus-relay: home link connected; awaiting AUTH";
}

void TunnelRelay::onAuthAccepted()
{
  drainPushQueue();
}

void TunnelRelay::postPushIntent(std::string payload)
{
  loop_.post([this, token = aliveToken_, payload = std::move(payload)] {
    if (token.expired() || stopped_.load())
      return;
    onPushIntent(payload);
  });
}

void TunnelRelay::onPushIntent(const std::string& payload)
{
  if (!pushQueue_.push(payload)) {
    LOG_WARN << "argus-relay: push intent dropped (full, empty or oversized; "
             << pushQueue_.dropped() << " total)";
    return;
  }
  drainPushQueue();
}

void TunnelRelay::drainPushQueue()
{
  while (!pushQueue_.empty()) {
    if (!mux_.homeActive())
      return;
    mux_.sendPush(pushQueue_.pop());
    pushForwarded_.fetch_add(1, std::memory_order_relaxed);
  }
}

void TunnelRelay::onLinkDown()
{
  if (!stopped_.load())
    LOG_WARN << "argus-relay: home link down; device streams torn down";
}

void TunnelRelay::onHomeAccepted(const PeerAcceptedInput& input)
{
  TcpPeer::Params params;
  params.loop = &loop_;
  params.fd = input.fd;
  params.ip = input.peerIp;
  params.port = input.peerPort;
  params.sndBuf = options_.limits.socketSndBuf;
  try {
    const TcpPeer::Ptr peer = TcpPeer::adopt(params);
    mux_.adoptHome(peer);
  } catch (const std::exception& error) {
    LOG_WARN << "argus-relay: home adopt failed: " << error.what();
    ::close(input.fd);
  }
}

void TunnelRelay::onDeviceAccepted(const PeerAcceptedInput& input)
{
  if (!mux_.homeActive()) {
    LOG_WARN << "argus-relay: device " << input.peerIp << ":"
             << input.peerPort
             << " rejected; home link not authenticated";
    ::close(input.fd);
    return;
  }
  const uint32_t streamId = mux_.openRemote();
  if (streamId == 0) {
    LOG_WARN << "argus-relay: device " << input.peerIp << ":"
             << input.peerPort
             << " rejected; stream cap reached or link down";
    ::close(input.fd);
    return;
  }
  TcpPeer::Params params;
  params.loop = &loop_;
  params.fd = input.fd;
  params.ip = input.peerIp;
  params.port = input.peerPort;
  params.sndBuf = options_.limits.socketSndBuf;
  try {
    const TcpPeer::Ptr peer = TcpPeer::adopt(params);
    if (!mux_.openLocal(streamId, peer))
      peer->close();
  } catch (const std::exception& error) {
    LOG_WARN << "argus-relay: device adopt failed: " << error.what();
    mux_.closeStream(streamId, CloseReason::Error);
    ::close(input.fd);
  }
}

void TunnelRelay::scheduleSweep()
{
  loop_.runAfter(1000, [this, token = aliveToken_] {
    if (token.expired() || stopped_.load())
      return;
    mux_.sweep();
    scheduleSweep();
  });
}
} // namespace tunnel
