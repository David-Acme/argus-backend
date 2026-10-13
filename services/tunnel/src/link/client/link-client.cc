#include <link/client/link-client.hxx>

#include <drogon/drogon.h>

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

namespace tunnel::link
{
namespace
{
constexpr std::chrono::seconds kSweepInterval{1};
constexpr std::chrono::seconds kDrainGrace{30};
}

int linkReconnectDelayMs(const LinkBackoffInput& input)
{
  const int base = std::max(1, input.baseMs);
  const int cap = std::max(base, input.capMs);
  const int doublings = std::clamp(input.attempt - 1, 0, 30);
  const auto window = static_cast<long long>(
      static_cast<unsigned long long>(base)
      << static_cast<unsigned>(std::min(doublings, 20)));
  const auto bounded = std::max<long long>(1, std::min<long long>(window, cap));
  const double jitter = std::clamp(input.jitter, 0.0, 1.0);
  return static_cast<int>(static_cast<double>(bounded) * jitter);
}

LinkClient::LinkClient(const Deps& deps)
    : loop_(deps.loop),
      options_(deps.options),
      observer_(deps.observer),
      tlsContext_({.certificatePath = options_.certificatePath,
                   .keyPath = options_.keyPath}),
      generator_(std::random_device{}()),
      pushQueue_(options_.pushQueueCapacity)
{
  localCapabilities_ = options_.capabilities & capability::kKnown;
}

LinkClient::~LinkClient()
{
  stop();
}

void LinkClient::start()
{
  loop_.post([this] {
    connectHome();
    scheduleSweep();
  });
}

void LinkClient::stop()
{
  if (stopped_.exchange(true))
    return;
  if (resolver_.joinable()) {
    resolver_.request_stop();
    resolver_.join();
  }
  loop_.post([this] { closeLink("shutting down"); });
}

void LinkClient::connectHome()
{
  if (stopped_.load() || channel_)
    return;
  if (resolver_.joinable())
    resolver_.join();
  const std::string host = options_.host;
  const uint16_t port = options_.port;
  resolver_ = std::jthread([this, host, port](const std::stop_token& token) {
    std::vector<Endpoint> endpoints = resolveHost(host, port);
    if (token.stop_requested() || stopped_.load())
      return;
    loop_.post([this, token2 = aliveToken_,
                resolved = std::move(endpoints)]() mutable {
      if (token2.expired() || stopped_.load())
        return;
      openChannel(resolved);
    });
  });
}

void LinkClient::openChannel(const std::vector<Endpoint>& endpoints)
{
  if (stopped_.load() || channel_)
    return;
  if (endpoints.empty()) {
    LOG_WARN << "argus-tunnel: cannot resolve the cloud node " << options_.host;
    scheduleReconnect();
    return;
  }
  TlsChannel::Params params;
  params.loop = &loop_;
  params.host = endpoints.front().ip;
  params.port = endpoints.front().port;
  params.serverName = options_.host;
  params.context = tlsContext_.handle();
  params.pinnedSubjectPublicKeyDer = options_.pinnedSubjectPublicKeyDer;
  params.sndBuf = options_.limits.socketSndBuf;
  try {
    channel_ = TlsChannel::connect(params);
  } catch (const std::exception& error) {
    LOG_WARN << "argus-tunnel: cannot dial the cloud node " << options_.host
             << ": " << error.what();
    scheduleReconnect();
    return;
  }
  installChannelCallbacks();
}

void LinkClient::installChannelCallbacks()
{
  TlsChannel::Callbacks callbacks;
  callbacks.onConnected = [this](TlsChannel&) {
    const auto now = Clock::now();
    attachedAt_ = now;
    lastFrameAt_ = now;
    lastPingAt_ = now;
    helloReceived_ = false;
    parser_ = FrameParser{};
    sendFrameToNode({.frame = encodeHello(localCapabilities_)});
    LOG_INFO << "argus-tunnel: TLS link to " << options_.host << ":"
             << options_.port << " established";
  };
  callbacks.onRead = [this](TlsChannel&, const char* data, size_t size) {
    handleLinkRead(data, size);
  };
  callbacks.onEof = [this](TlsChannel&) {
    closeLink("the cloud node closed the link");
  };
  callbacks.onDrained = [this](TlsChannel&) {
    std::vector<uint32_t> ids;
    ids.reserve(streams_.size());
    for (const auto& [id, stream] : streams_) {
      (void)stream;
      ids.push_back(id);
    }
    for (uint32_t id : ids)
      flushToHome(id);
    resumeLocalReads();
    resumeLinkRead();
  };
  callbacks.onClosed = [this](TlsChannel&) { closeLink("the link broke"); };
  if (channel_)
    channel_->setCallbacks(std::move(callbacks));
}

void LinkClient::closeLink(std::string_view reason)
{
  if (!channel_)
    return;
  const bool wasUp = connected_.load();
  TlsChannel::Ptr channel = std::move(channel_);
  channel->setCallbacks(TlsChannel::Callbacks{});
  channel->close();
  parser_ = FrameParser{};
  teardownStreams();
  helloReceived_ = false;
  connected_.store(false);
  draining_.store(false);
  activeKeepalive_.store(false);
  awaitingPong_ = false;
  linkReadPaused_ = false;
  negotiatedCredit_ = false;
  remoteCapabilities_.store(0);
  if (wasUp)
    LOG_INFO << "argus-tunnel: the link to the cloud is down: " << reason;
  if (observer_ != nullptr)
    observer_->onLinkDown();
  scheduleReconnect();
}

void LinkClient::refuseLink(WireError error)
{
  LOG_WARN << "argus-tunnel: the cloud node violated the wire: "
           << wireErrorToString(error);
  if (observer_ != nullptr)
    observer_->onWireError(error);
  sendFrameToNode(
      {.frame = encodeGoAway(GoAwayReason::ProtocolError, 0)});
  closeLink(wireErrorToString(error));
}

void LinkClient::handleLinkRead(const char* data, size_t size)
{
  parser_.feed(std::string_view{data, size});
  if (parser_.failed()) {
    refuseLink(parser_.error());
    return;
  }
  pumpFrames();
}

void LinkClient::pumpFrames()
{
  while (parser_.hasFrame() && !parser_.failed() && !linkReadPaused_)
    dispatchFrame(parser_.popFrame());
}

void LinkClient::dispatchFrame(const Frame& frame)
{
  lastFrameAt_ = Clock::now();
  if (frame.type != FrameType::Hello && !helloReceived_) {
    refuseLink(WireError::HelloRequired);
    return;
  }
  switch (frame.type) {
  case FrameType::Hello:
    handleHello(frame);
    break;
  case FrameType::Open:
    handleOpen(frame);
    break;
  case FrameType::Data:
    handleData(frame);
    break;
  case FrameType::Close:
    handleClose(frame);
    break;
  case FrameType::Ping:
    handlePing(frame);
    break;
  case FrameType::Pong:
    handlePong(frame);
    break;
  case FrameType::Push:
    handlePush(frame);
    break;
  case FrameType::GoAway:
    handleGoAway(frame);
    break;
  case FrameType::Credit:
    handleCredit(frame);
    break;
  }
}

void LinkClient::handleHello(const Frame& frame)
{
  if (helloReceived_) {
    refuseLink(WireError::DuplicateHello);
    return;
  }
  const HelloPayload hello = decodeHello(frame);
  const uint32_t remote = hello.capabilities & capability::kKnown;
  remoteCapabilities_.store(remote);
  negotiatedCredit_ =
      (remote & capability::kStreamCredit) != 0 &&
      (localCapabilities_ & capability::kStreamCredit) != 0;
  helloReceived_ = true;
  connected_.store(true);
  attempts_.store(0);
  LOG_INFO << "argus-tunnel: home link up with " << options_.host << ":"
           << options_.port << "; the node offers " << remote
           << (negotiatedCredit_ ? " (stream credit negotiated)" : "");
  if (observer_ != nullptr)
    observer_->onLinkUp();
}

void LinkClient::handleOpen(const Frame& frame)
{
  if (frame.streamId == 0 || streams_.contains(frame.streamId))
    return;
  openStream(frame.streamId);
}

void LinkClient::openStream(uint32_t streamId)
{
  if (streams_.size() >= options_.limits.maxStreams) {
    sendFrameToNode({.frame = encodeClose(streamId, CloseReason::Busy)});
    return;
  }
  Stream& stream = streams_[streamId];
  stream.id = streamId;
  stream.lastActivity = Clock::now();
  stream.sendWindow =
      CreditWindow({.limited = negotiatedCredit_,
                    .window = kInitialStreamCredit,
                    .grantThreshold = kCreditGrantThreshold});
  stream.receiveWindow =
      CreditWindow({.limited = negotiatedCredit_,
                    .window = kInitialStreamCredit,
                    .grantThreshold = kCreditGrantThreshold});
  streamCount_.store(streams_.size());

  TcpPeer::Params params;
  params.loop = &loop_;
  params.ip = options_.gatewayHost;
  params.port = options_.gatewayPort;
  params.sndBuf = options_.limits.socketSndBuf;
  params.callbacks.onConnected = [this, streamId](TcpPeer&) {
    Stream* target = findStream(streamId);
    if (target != nullptr)
      target->localAttached = true;
  };
  params.callbacks.onRead = [this, streamId](TcpPeer&, const char* data,
                                             size_t size) {
    if (findStream(streamId) != nullptr)
      writeToStream(streamId, data, size);
  };
  params.callbacks.onEof = [this, streamId](TcpPeer&) {
    if (findStream(streamId) != nullptr)
      handleLocalEof(streamId);
  };
  params.callbacks.onDrained = [this, streamId](TcpPeer& peer) {
    Stream* target = findStream(streamId);
    if (target != nullptr && target->local.get() == &peer)
      flushToLocal(streamId);
  };
  params.callbacks.onClosed = [this, streamId](TcpPeer& peer) {
    Stream* target = findStream(streamId);
    if (target == nullptr || target->local.get() != &peer)
      return;
    closeStream(streamId,
                target->localAttached ? CloseReason::Error : CloseReason::Busy);
  };
  try {
    const TcpPeer::Ptr local = TcpPeer::connect(params);
    Stream* target = findStream(streamId);
    if (target != nullptr)
      target->local = local;
  } catch (const std::exception& error) {
    LOG_WARN << "argus-tunnel: cannot reach the home remote listener on "
             << options_.gatewayHost << ":" << options_.gatewayPort << ": "
             << error.what();
    sendFrameToNode({.frame = encodeClose(streamId, CloseReason::Busy)});
    streams_.erase(streamId);
    streamCount_.store(streams_.size());
  }
}

void LinkClient::handleData(const Frame& frame)
{
  Stream* stream = findStream(frame.streamId);
  if (stream == nullptr)
    return;
  stream->lastActivity = Clock::now();
  const size_t size = frame.payload.size();
  if (!stream->receiveWindow.covers(size)) {
    LOG_WARN << "argus-tunnel: the cloud node exceeded the credit window of "
             << "stream " << frame.streamId;
    if (observer_ != nullptr)
      observer_->onWireError(WireError::OverCredit);
    closeStream(frame.streamId, CloseReason::Backpressure);
    return;
  }
  stream->receiveWindow.recordUsed(size);
  if (!stream->local || !stream->pendingToLocal.empty() ||
      stream->local->congested()) {
    if (stream->pendingToLocal.size() + size >
        2 * options_.limits.streamPendingCap) {
      closeStream(frame.streamId, CloseReason::Backpressure);
      return;
    }
    stream->pendingToLocal.append(frame.payload);
    globalPendingToLocal_ += size;
    updatePending();
    if (globalPendingToLocal_ >= options_.limits.linkHighWater &&
        !linkReadPaused_ && channel_) {
      linkReadPaused_ = true;
      channel_->setReadPaused(true);
    }
    return;
  }
  TcpPeer::Ptr local = stream->local;
  local->send(frame.payload);
  Stream* delivered = findStream(frame.streamId);
  if (delivered == nullptr)
    return;
  grantConsumedCredit(*delivered, size);
}

void LinkClient::handleClose(const Frame& frame)
{
  Stream* stream = findStream(frame.streamId);
  if (stream == nullptr)
    return;
  stream->closedByNode = true;
  closeLocal(*stream, decodeCloseReason(frame));
}

void LinkClient::handlePing(const Frame& frame)
{
  sendFrameToNode({.frame = encodePong(decodeNonce(frame))});
}

void LinkClient::handlePong(const Frame& frame)
{
  if (decodeNonce(frame) == pingNonce_)
    awaitingPong_ = false;
}

void LinkClient::handlePush(const Frame& frame)
{
  if ((remoteCapabilities_.load() & capability::kPushIntents) == 0 ||
      (localCapabilities_ & capability::kPushIntents) == 0) {
    LOG_WARN << "argus-tunnel: the cloud node sent a push intent on a link "
                "that did not negotiate them";
    sendFrameToNode({.frame = encodeGoAway(GoAwayReason::ProtocolError, 0)});
    closeLink("push intent without the capability");
    return;
  }
  if (observer_ != nullptr)
    observer_->onPushIntent(frame.payload);
  if (pushQueue_.push(frame.payload))
    LOG_INFO << "argus-tunnel: push intent queued (" << pushQueue_.size()
             << " buffered)";
  else
    LOG_WARN << "argus-tunnel: push queue full; intent dropped ("
             << pushQueue_.dropped() << " total)";
}

void LinkClient::handleGoAway(const Frame& frame)
{
  const GoAwayPayload goAway = decodeGoAway(frame);
  draining_.store(true);
  reconnectWithoutBackoff_ = true;
  LOG_INFO << "argus-tunnel: the cloud node is going away ("
           << goAwayReasonToString(goAway.reason) << ", " << goAway.seconds
           << "s); the link ends when its streams do and reconnects at once";
  if (observer_ != nullptr)
    observer_->onGoingAway(goAway.reason, goAway.seconds);
}

void LinkClient::handleCredit(const Frame& frame)
{
  Stream* stream = findStream(frame.streamId);
  if (stream == nullptr)
    return;
  stream->sendWindow.grant(decodeCredit(frame));
  flushToHome(frame.streamId);
}

LinkClient::Stream* LinkClient::findStream(uint32_t streamId)
{
  const auto it = streams_.find(streamId);
  return it == streams_.end() ? nullptr : &it->second;
}

void LinkClient::writeToStream(uint32_t streamId, const char* data, size_t size)
{
  Stream* stream = findStream(streamId);
  if (stream == nullptr || size == 0)
    return;
  if (!helloReceived_ || !channel_ || channel_->closed()) {
    closeStream(streamId, CloseReason::Error);
    return;
  }
  stream->lastActivity = Clock::now();
  stream->pendingToHome.append(data, size);
  globalPendingToHome_ += size;
  updatePending();
  if (stream->pendingToHome.size() >= options_.limits.streamPendingCap &&
      !stream->localReadPaused && stream->local) {
    stream->localReadPaused = true;
    stream->local->setReadPaused(true);
  }
  if (globalPendingToHome_ >= options_.limits.globalPendingCap)
    pauseAllLocalReads();
  flushToHome(streamId);
}

void LinkClient::handleLocalEof(uint32_t streamId)
{
  Stream* stream = findStream(streamId);
  if (stream == nullptr)
    return;
  stream->localEof = true;
  if (stream->pendingToHome.empty())
    closeStream(streamId, CloseReason::Normal);
}

void LinkClient::flushToHome(uint32_t streamId)
{
  while (true) {
    Stream* stream = findStream(streamId);
    if (stream == nullptr || !channel_ || channel_->closed())
      return;
    if (stream->pendingToHome.empty()) {
      if (stream->localEof) {
        closeStream(streamId, CloseReason::Normal);
        resumeLocalReads();
        return;
      }
      if (stream->localReadPaused &&
          globalPendingToHome_ < options_.limits.globalPendingCap / 2 &&
          stream->local) {
        stream->localReadPaused = false;
        stream->local->setReadPaused(false);
      }
      resumeLocalReads();
      return;
    }
    if (channel_->congested())
      return;
    size_t chunk = std::min(stream->pendingToHome.size(), kDataFrameMax);
    if (stream->sendWindow.limited()) {
      const uint32_t available = stream->sendWindow.remaining();
      if (available == 0)
        return;
      chunk = std::min<size_t>(chunk, available);
    }
    const std::string_view bytes{stream->pendingToHome.data(), chunk};
    sendFrameToNode({.frame = encodeData(streamId, bytes)});
    stream = findStream(streamId);
    if (stream == nullptr)
      return;
    stream->sendWindow.recordUsed(chunk);
    stream->pendingToHome.erase(0, chunk);
    globalPendingToHome_ -= chunk;
    updatePending();
  }
}

void LinkClient::flushToLocal(uint32_t streamId)
{
  while (true) {
    Stream* stream = findStream(streamId);
    if (stream == nullptr || !stream->local || stream->local->closed())
      return;
    if (stream->pendingToLocal.empty()) {
      resumeLinkRead();
      return;
    }
    if (stream->local->congested())
      return;
    const size_t size = stream->pendingToLocal.size();
    TcpPeer::Ptr local = stream->local;
    local->send(std::move(stream->pendingToLocal));
    stream = findStream(streamId);
    if (stream == nullptr)
      return;
    stream->pendingToLocal.clear();
    globalPendingToLocal_ -= size;
    updatePending();
    grantConsumedCredit(*stream, size);
  }
}

void LinkClient::grantConsumedCredit(Stream& stream, size_t consumedBytes)
{
  stream.receiveWindow.markConsumed(consumedBytes);
  const uint32_t granted = stream.receiveWindow.takePendingGrant();
  if (granted > 0) {
    sendFrameToNode(
        {.frame = encodeCredit({.streamId = stream.id, .bytes = granted})});
  }
}

void LinkClient::closeStream(uint32_t streamId, CloseReason reason)
{
  Stream* stream = findStream(streamId);
  if (stream == nullptr)
    return;
  closeLocal(*stream, reason);
}

void LinkClient::closeLocal(Stream& stream, CloseReason reason)
{
  const uint32_t id = stream.id;
  Stream moved = std::move(stream);
  streams_.erase(id);
  streamCount_.store(streams_.size());
  globalPendingToHome_ -= moved.pendingToHome.size();
  globalPendingToLocal_ -= moved.pendingToLocal.size();
  updatePending();
  if (helloReceived_ && !moved.closedByNode)
    sendFrameToNode({.frame = encodeClose(id, reason)});
  if (moved.local)
    releaseLocal(moved, reason);
  resumeLinkRead();
}

void LinkClient::releaseLocal(Stream& stream, CloseReason reason)
{
  const TcpPeer::Ptr local = stream.local;
  local->setCallbacks(TcpPeer::Callbacks{});
  if (reason != CloseReason::Normal || local->closed()) {
    local->close();
    return;
  }
  if (!stream.pendingToLocal.empty())
    local->send(std::move(stream.pendingToLocal));
  local->closeWhenFlushed();
  if (!local->closed())
    drainingPeers_.push_back({.peer = local,
                              .deadline = Clock::now() + kDrainGrace});
}

void LinkClient::teardownStreams()
{
  while (!streams_.empty())
    closeLocal(streams_.begin()->second, CloseReason::Error);
  globalPendingToHome_ = 0;
  globalPendingToLocal_ = 0;
  updatePending();
}

void LinkClient::pauseAllLocalReads()
{
  for (auto& [id, stream] : streams_) {
    (void)id;
    if (stream.local && !stream.localReadPaused) {
      stream.localReadPaused = true;
      stream.local->setReadPaused(true);
    }
  }
}

void LinkClient::resumeLocalReads()
{
  if (globalPendingToHome_ > options_.limits.globalPendingCap / 2)
    return;
  for (auto& [id, stream] : streams_) {
    (void)id;
    if (stream.local && stream.localReadPaused &&
        stream.pendingToHome.empty()) {
      stream.localReadPaused = false;
      stream.local->setReadPaused(false);
    }
  }
}

void LinkClient::resumeLinkRead()
{
  if (!linkReadPaused_ || !channel_)
    return;
  if (globalPendingToLocal_ <= options_.limits.linkHighWater / 2) {
    linkReadPaused_ = false;
    channel_->setReadPaused(false);
    pumpFrames();
  }
}

void LinkClient::sendFrameToNode(const FrameToNodeInput& input)
{
  if (!channel_ || channel_->closed() || input.frame.empty())
    return;
  channel_->send(input.frame);
}

void LinkClient::updatePending()
{
  pendingBytes_.store(globalPendingToHome_ + globalPendingToLocal_);
}

void LinkClient::scheduleSweep()
{
  loop_.runAfter(static_cast<int>(kSweepInterval.count()) * 1000,
                 [this, token = aliveToken_] {
                   if (token.expired() || stopped_.load())
                     return;
                   sweep();
                   scheduleSweep();
                 });
}

void LinkClient::scheduleReconnect()
{
  if (stopped_.load() || reconnectPending_)
    return;
  reconnectPending_ = true;
  const bool immediate = reconnectWithoutBackoff_;
  reconnectWithoutBackoff_ = false;
  if (immediate) {
    attempts_.store(0);
    lastDelayMs_.store(0);
    armReconnect(0);
    return;
  }
  armReconnect(nextDelayMs());
}

int LinkClient::nextDelayMs()
{
  const int attempt = attempts_.load() + 1;
  attempts_.store(attempt);
  std::uniform_real_distribution<double> distribution{0.0, 1.0};
  const int delay = linkReconnectDelayMs({.attempt = attempt,
                                          .baseMs = options_.reconnectBaseMs,
                                          .capMs = options_.reconnectCapMs,
                                          .jitter = distribution(generator_)});
  lastDelayMs_.store(delay);
  if (attempt > 1 && delay >= options_.reconnectCapMs)
    LOG_WARN << "argus-tunnel: the cloud is still unreachable after " << attempt
             << " attempts; retrying every " << delay << " ms";
  return delay;
}

void LinkClient::armReconnect(int delayMs)
{
  loop_.runAfter(delayMs, [this, token = aliveToken_] {
    if (token.expired() || stopped_.load())
      return;
    reconnectPending_ = false;
    connectHome();
  });
}

void LinkClient::sweep()
{
  const auto now = Clock::now();
  if (channel_ && !connected_.load() &&
      now - attachedAt_ > options_.helloTimeout) {
    closeLink("hello timeout");
  } else if (channel_ && connected_.load() &&
             now - lastFrameAt_ > options_.deadLinkTimeout) {
    closeLink("dead link");
  } else if (channel_ && connected_.load()) {
    keepaliveTick();
  }
  std::vector<uint32_t> idle;
  for (auto& [id, stream] : streams_) {
    if (now - stream.lastActivity > options_.limits.streamIdleTimeout)
      idle.push_back(id);
  }
  for (uint32_t id : idle)
    closeStream(id, CloseReason::IdleTimeout);
  std::erase_if(drainingPeers_, [now](const DrainingPeer& draining) {
    if (draining.peer->closed())
      return true;
    if (now < draining.deadline)
      return false;
    draining.peer->close();
    return true;
  });
}

void LinkClient::keepaliveTick()
{
  const auto now = Clock::now();
  if (awaitingPong_ && now - lastPingAt_ > options_.keepalivePongTimeout) {
    LOG_WARN << "argus-tunnel: the cloud node did not answer a ping within "
             << options_.keepalivePongTimeout.count() << "s";
    closeLink("pong timeout");
    return;
  }
  const bool active = !streams_.empty();
  activeKeepalive_.store(active);
  const auto interval =
      active ? options_.keepaliveActive : options_.keepaliveIdle;
  if (now - lastPingAt_ < interval)
    return;
  pingNonce_ = generator_();
  awaitingPong_ = true;
  lastPingAt_ = now;
  sendFrameToNode({.frame = encodePing(pingNonce_)});
}
}
