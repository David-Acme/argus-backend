#include "tunnel-client.hxx"

#include <drogon/drogon.h>

#include <algorithm>
#include <utility>

namespace tunnel
{
int reconnectDelayMs(const BackoffInput& input)
{
  const int base = std::max(1, input.baseMs);
  const int cap = std::max(base, input.maxMs);
  const int doublings = std::clamp(input.attempt - 1, 0, 30);
  const auto delay = static_cast<long long>(
      static_cast<unsigned long long>(base) << static_cast<unsigned>(std::min(doublings, 20)));
  return static_cast<int>(std::min<long long>(delay, cap));
}

TunnelClient::TunnelClient(PollLoop& loop, ClientOptions options)
    : loop_(loop), options_(std::move(options)),
      mux_({.loop = loop, .limits = options_.limits, .delegate = this})
{
}

TunnelClient::~TunnelClient()
{
  stop();
}

void TunnelClient::start()
{
  loop_.post([this] {
    connectHome();
    scheduleSweep();
    schedulePing();
  });
}

void TunnelClient::stop()
{
  if (stopped_.exchange(true))
    return;
  loop_.post([this] {
    closePending();
    mux_.dropLink();
  });
}

void TunnelClient::closePending()
{
  if (pendingHome_) {
    pendingHome_->setCallbacks(TcpPeer::Callbacks{});
    pendingHome_->close();
    pendingHome_.reset();
  }
  for (auto& [id, peer] : pendingDials_) {
    peer->setCallbacks(TcpPeer::Callbacks{});
    peer->close();
  }
  pendingDials_.clear();
}

void TunnelClient::connectHome()
{
  if (stopped_.load())
    return;
  TcpPeer::Params params;
  params.loop = &loop_;
  params.ip = options_.relayHost;
  params.port = options_.relayPort;
  params.sndBuf = options_.limits.socketSndBuf;
  params.callbacks.onConnected = [this](TcpPeer& peer) {
    static_cast<void>(mux_.adoptHome(peer.sharedFromThis()));
    pendingHome_.reset();
    homeConnected_.store(true);
  };
  params.callbacks.onClosed = [this](TcpPeer&) {
    pendingHome_.reset();
    homeConnected_.store(false);
    scheduleReconnect();
  };
  try {
    pendingHome_ = TcpPeer::connect(params);
  } catch (const std::exception& error) {
    LOG_WARN << "argus-tunnel: relay connect failed: " << error.what();
    scheduleReconnect();
  }
}

void TunnelClient::onLinkUp()
{
  homeConnected_.store(true);
}

void TunnelClient::onLinkDown()
{
  homeConnected_.store(false);
  scheduleReconnect();
}

void TunnelClient::onAuthAccepted()
{
  reconnectAttempts_.store(0);
  LOG_INFO << "argus-tunnel: home link active (relay " << options_.relayHost
           << ":" << options_.relayPort << ")";
}

void TunnelClient::onAuthRejected()
{
  LOG_ERROR << "argus-tunnel: relay rejected the [tunnel] secret; check the "
               "configuration on both sides";
}

void TunnelClient::onPushFrame(const std::string& payload)
{
  if (pushQueue_.push(payload))
    LOG_INFO << "argus-tunnel: push intent queued (" << pushQueue_.size()
             << " buffered)";
  else
    LOG_WARN << "argus-tunnel: push queue full; intent dropped ("
             << pushQueue_.dropped() << " total)";
}

void TunnelClient::onRemoteOpen(uint32_t streamId)
{
  TcpPeer::Params params;
  params.loop = &loop_;
  params.ip = options_.gatewayHost;
  params.port = options_.gatewayPort;
  params.sndBuf = options_.limits.socketSndBuf;
  params.callbacks.onConnected = [this, streamId](TcpPeer& peer) {
    if (mux_.openLocal(streamId, peer.sharedFromThis()))
      pendingDials_.erase(streamId);
    else
      peer.close();
  };
  params.callbacks.onClosed = [this, streamId](TcpPeer& peer) {
    pendingDials_.erase(streamId);
    if (!peer.connected())
      mux_.closeStream(streamId, CloseReason::Error);
  };
  try {
    pendingDials_[streamId] = TcpPeer::connect(params);
  } catch (const std::exception& error) {
    LOG_WARN << "argus-tunnel: gateway dial failed: " << error.what();
    mux_.closeStream(streamId, CloseReason::Error);
  }
}

void TunnelClient::scheduleReconnect()
{
  if (stopped_.load() || reconnectPending_)
    return;
  reconnectPending_ = true;
  const int attempts = reconnectAttempts_.load() + 1;
  reconnectAttempts_.store(attempts);
  const int delay = reconnectDelayMs({.attempt = attempts,
                                      .baseMs = options_.reconnectWaitMs,
                                      .maxMs = options_.reconnectMaxWaitMs});
  lastReconnectDelayMs_.store(delay);
  if (attempts > 1 && delay >= options_.reconnectMaxWaitMs)
    LOG_WARN << "argus-tunnel: relay still unreachable after " << attempts
             << " attempts; retrying every " << delay << " ms";
  loop_.runAfter(delay, [this, token = aliveToken_] {
    if (token.expired() || stopped_.load())
      return;
    reconnectPending_ = false;
    connectHome();
  });
}

void TunnelClient::scheduleSweep()
{
  loop_.runAfter(1000, [this, token = aliveToken_] {
    if (token.expired() || stopped_.load())
      return;
    mux_.sweep();
    scheduleSweep();
  });
}

void TunnelClient::schedulePing()
{
  const int waitMs = options_.pingIntervalSeconds > 0
                         ? options_.pingIntervalSeconds * 1000
                         : 30000;
  loop_.runAfter(waitMs, [this, token = aliveToken_] {
    if (token.expired() || stopped_.load())
      return;
    if (mux_.homeActive())
      mux_.sendPing();
    schedulePing();
  });
}
}
