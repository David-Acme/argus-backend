#include "tunnel-mux.hxx"

#include <drogon/drogon.h>

#include <algorithm>
#include <vector>

namespace tunnel
{
namespace
{
constexpr std::chrono::seconds kDrainGrace{30};
}

TunnelMux::TunnelMux(const Deps& deps)
    : loop_(deps.loop), limits_(deps.limits), delegate_(deps.delegate)
{
}

Stream* TunnelMux::findStream(uint32_t streamId)
{
  const auto it = streams_.find(streamId);
  return it == streams_.end() ? nullptr : &it->second;
}

bool TunnelMux::adoptHome(const TcpPeer::Ptr& peer)
{
  if (delegate_->validatesAuth())
    return adoptRelayCandidate(peer);
  if (homePeer_)
    dropLink();
  homePeer_ = peer;
  parser_ = FrameParser();
  authChallenge_.clear();
  homeActive_ = false;
  homeReadPaused_ = false;
  installHomeCallbacks(peer);
  const auto now = Clock::now();
  attachedAt_ = now;
  lastFrameAt_ = now;
  delegate_->onLinkUp();
  return true;
}

bool TunnelMux::adoptRelayCandidate(const TcpPeer::Ptr& peer)
{
  if (homePeer_ && homeActive_ &&
      Clock::now() - lastFrameAt_ < limits_.deadLinkTimeout)
    return false;
  if (static_cast<int>(pendingHomes_.size()) >= limits_.maxPendingHomes)
    return false;
  const std::string& ip = peer->peerIp();
  const auto sameIp = std::ranges::count_if(pendingHomes_, [&ip](const auto& entry) {
    return entry.second.peer->peerIp() == ip;
  });
  if (sameIp >= limits_.maxPendingHomesPerIp)
    return false;

  TcpPeer* key = peer.get();
  PendingHome& pending = pendingHomes_[key];
  pending.peer = peer;
  pending.challenge = randomChallenge();
  pending.attachedAt = Clock::now();
  TcpPeer::Callbacks callbacks;
  callbacks.onRead = [this, key](TcpPeer&, const char* data, size_t size) {
    handlePendingRead({.key = key, .data = data, .size = size});
  };
  callbacks.onEof = [this, key](TcpPeer&) {
    if (homePeer_.get() == key)
      dropLink();
    else
      dropPendingHome(key);
  };
  callbacks.onClosed = [this, key](TcpPeer&) {
    if (homePeer_.get() == key)
      dropLink();
    else
      dropPendingHome(key);
  };
  peer->setCallbacks(std::move(callbacks));
  peer->send(encodeFrame({.type = FrameType::Challenge,
                          .streamId = 0,
                          .payload = pending.challenge.data(),
                          .size = pending.challenge.size()}));
  return true;
}

void TunnelMux::installHomeCallbacks(const TcpPeer::Ptr& peer)
{
  TcpPeer::Callbacks callbacks;
  callbacks.onRead = [this](TcpPeer&, const char* data, size_t size) {
    handleHomeRead({.data = data, .size = size});
  };
  callbacks.onEof = [this](TcpPeer&) {
    if (homePeer_ && !homePeer_->closed())
      dropLink();
  };
  callbacks.onDrained = [this](TcpPeer&) { handleHomeDrained(*homePeer_); };
  callbacks.onClosed = [this](TcpPeer&) {
    if (homePeer_ && homePeer_->closed())
      dropLink();
  };
  peer->setCallbacks(std::move(callbacks));
}

void TunnelMux::handlePendingRead(const PendingReadInput& input)
{
  const auto it = pendingHomes_.find(input.key);
  if (it == pendingHomes_.end()) {
    if (homePeer_.get() == input.key)
      handleHomeRead({.data = input.data, .size = input.size});
    return;
  }
  PendingHome& pending = it->second;
  pending.parser.feed(input.data, input.size);
  if (pending.parser.failed()) {
    dropPendingHome(input.key);
    return;
  }
  if (!pending.parser.hasFrame())
    return;
  const Frame frame = pending.parser.popFrame();
  const bool proven =
      frame.type == FrameType::Auth &&
      constantTimeEquals(frame.payload,
                         authMac(delegate_->authSecret(), pending.challenge));
  if (!proven) {
    pending.peer->send(encodeFrame({.type = FrameType::AuthFail,
                                    .streamId = 0,
                                    .payload = nullptr,
                                    .size = 0}));
    dropPendingHome(input.key);
    return;
  }
  promotePendingHome(input.key);
}

void TunnelMux::promotePendingHome(TcpPeer* key)
{
  const auto it = pendingHomes_.find(key);
  if (it == pendingHomes_.end())
    return;
  PendingHome promoted = std::move(it->second);
  pendingHomes_.erase(it);
  if (homePeer_)
    dropLink();
  homePeer_ = promoted.peer;
  parser_ = std::move(promoted.parser);
  authChallenge_ = std::move(promoted.challenge);
  homeActive_ = true;
  homeReadPaused_ = false;
  installHomeCallbacks(homePeer_);
  const auto now = Clock::now();
  attachedAt_ = now;
  lastFrameAt_ = now;
  delegate_->onLinkUp();
  const std::string proof =
      relayAuthMac(delegate_->authSecret(), authChallenge_);
  sendFrameToHome({.type = FrameType::AuthOk,
                   .streamId = 0,
                   .payload = proof.data(),
                   .size = proof.size()});
  delegate_->onAuthAccepted();
  pumpHomeFrames();
}

void TunnelMux::dropPendingHome(TcpPeer* key)
{
  const auto it = pendingHomes_.find(key);
  if (it == pendingHomes_.end())
    return;
  const TcpPeer::Ptr peer = it->second.peer;
  pendingHomes_.erase(it);
  peer->setCallbacks(TcpPeer::Callbacks{});
  peer->close();
}

void TunnelMux::dropPendingHomes()
{
  while (!pendingHomes_.empty())
    dropPendingHome(pendingHomes_.begin()->first);
}

void TunnelMux::sendAuth()
{
  if (!homePeer_ || authChallenge_.empty())
    return;
  const std::string mac = authMac(delegate_->authSecret(), authChallenge_);
  sendFrameToHome({.type = FrameType::Auth,
                      .streamId = 0,
                      .payload = mac.data(),
                      .size = mac.size()});
}

uint32_t TunnelMux::openRemote()
{
  if (!homePeer_ || !homeActive_)
    return 0;
  if (static_cast<int>(streams_.size()) >= limits_.maxStreams)
    return 0;
  uint32_t id = nextStreamId_++;
  while (id == 0 || streams_.contains(id))
    id = nextStreamId_++;
  Stream& stream = streams_[id];
  stream.id = id;
  stream.lastActivity = Clock::now();
  sendFrameToHome({.type = FrameType::Open,
                      .streamId = id,
                      .payload = nullptr,
                      .size = 0});
  return id;
}

bool TunnelMux::openLocal(uint32_t streamId, const TcpPeer::Ptr& peer)
{
  Stream* stream = findStream(streamId);
  if (!stream || stream->local)
    return false;
  stream->local = peer;
  TcpPeer::Callbacks callbacks;
  callbacks.onRead = [this, streamId](TcpPeer&, const char* data,
                                      size_t size) {
    Stream* target = findStream(streamId);
    if (target)
      handleLocalRead({.stream = *target, .data = data, .size = size});
  };
  callbacks.onEof = [this, streamId](TcpPeer&) {
    Stream* target = findStream(streamId);
    if (target)
      handleLocalEof(*target);
  };
  callbacks.onDrained = [this, streamId](TcpPeer& peer) {
    Stream* target = findStream(streamId);
    if (target && target->local.get() == &peer)
      flushToLocal(*target);
  };
  callbacks.onClosed = [this, streamId](TcpPeer&) {
    Stream* target = findStream(streamId);
    if (target && target->local && target->local->closed())
      closeStream(streamId, CloseReason::Error);
  };
  peer->setCallbacks(std::move(callbacks));
  stream->lastActivity = Clock::now();
  flushToLocal(*stream);
  return true;
}

void TunnelMux::closeStream(uint32_t streamId, CloseReason reason)
{
  Stream* stream = findStream(streamId);
  if (!stream)
    return;
  closeLocal(*stream, reason);
}

void TunnelMux::dropLink()
{
  if (!homePeer_)
    return;
  const TcpPeer::Ptr peer = homePeer_;
  homePeer_.reset();
  homeActive_ = false;
  homeReadPaused_ = false;
  parser_ = FrameParser();
  authChallenge_.clear();
  peer->setCallbacks(TcpPeer::Callbacks{});
  peer->close();
  teardownAll();
  delegate_->onLinkDown();
}

void TunnelMux::teardownAll()
{
  while (!streams_.empty())
    closeStream(streams_.begin()->first, CloseReason::Error);
  globalPendingToHome_ = 0;
  globalPendingToLocal_ = 0;
}

void TunnelMux::sendPing()
{
  if (!homePeer_)
    return;
  sendFrameToHome({.type = FrameType::Ping,
                      .streamId = 0,
                      .payload = nullptr,
                      .size = 0});
}

bool TunnelMux::sendPush(const std::string& payload)
{
  if (!homePeer_ || !homeActive_ || payload.empty() ||
      payload.size() > kMaxPayload)
    return false;
  sendFrameToHome({.type = FrameType::Push,
                      .streamId = 0,
                      .payload = payload.data(),
                      .size = payload.size()});
  return true;
}

void TunnelMux::sweep()
{
  const auto now = Clock::now();
  std::vector<TcpPeer*> stale;
  for (const auto& [key, pending] : pendingHomes_) {
    if (now - pending.attachedAt > limits_.authTimeout)
      stale.push_back(key);
  }
  for (TcpPeer* key : stale)
    dropPendingHome(key);
  if (homePeer_ && !homeActive_ && now - attachedAt_ > limits_.authTimeout)
    dropLink();
  else if (homePeer_ && homeActive_ &&
           now - lastFrameAt_ > limits_.deadLinkTimeout)
    dropLink();
  std::vector<uint32_t> idle;
  for (auto& [id, stream] : streams_) {
    if (now - stream.lastActivity > limits_.idleTimeout)
      idle.push_back(id);
  }
  for (uint32_t id : idle)
    closeStream(id, CloseReason::IdleTimeout);
  std::erase_if(draining_, [now](const DrainingPeer& draining) {
    if (draining.peer->closed())
      return true;
    if (now < draining.deadline)
      return false;
    draining.peer->close();
    return true;
  });
}

void TunnelMux::handleHomeRead(const HomeReadInput& input)
{
  parser_.feed(input.data, input.size);
  if (parser_.failed()) {
    LOG_WARN << "argus-tunnel: home link frame desync; dropping link";
    dropLink();
    return;
  }
  pumpHomeFrames();
}

void TunnelMux::pumpHomeFrames()
{
  while (parser_.hasFrame() && !parser_.failed() && !homeReadPaused_)
    dispatchFrame(parser_.popFrame());
}

void TunnelMux::handleHomeDrained(TcpPeer&)
{
  std::vector<uint32_t> ids;
  ids.reserve(streams_.size());
  for (auto& [id, stream] : streams_)
    ids.push_back(id);
  for (uint32_t id : ids) {
    Stream* stream = findStream(id);
    if (stream)
      flushToHome(*stream);
  }
  resumeLocalReads();
  resumeHomeRead();
}

void TunnelMux::dispatchFrame(Frame frame)
{
  lastFrameAt_ = Clock::now();
  switch (frame.type) {
  case FrameType::Auth:
    if (delegate_->validatesAuth())
      LOG_WARN << "argus-tunnel: AUTH frame on an authenticated home link";
    else
      LOG_WARN << "argus-tunnel: unexpected AUTH frame from the relay";
    dropLink();
    break;
  case FrameType::AuthOk:
    if (homeActive_)
      break;
    if (constantTimeEquals(frame.payload,
                           relayAuthMac(delegate_->authSecret(),
                                        authChallenge_))) {
      homeActive_ = true;
      delegate_->onAuthAccepted();
    } else {
      LOG_WARN << "argus-tunnel: relay failed the AUTH proof; dropping link";
      dropLink();
    }
    break;
  case FrameType::AuthFail:
    LOG_WARN << "argus-tunnel: relay rejected the tunnel secret";
    dropLink();
    delegate_->onAuthRejected();
    break;
  case FrameType::Challenge:
    if (delegate_->validatesAuth()) {
      LOG_WARN << "argus-tunnel: unexpected CHALLENGE frame from the client";
      dropLink();
      break;
    }
    if (homeActive_)
      break;
    if (frame.payload.size() != kChallengeSize) {
      LOG_WARN << "argus-tunnel: malformed AUTH challenge; dropping link";
      dropLink();
      break;
    }
    authChallenge_ = frame.payload;
    sendAuth();
    break;
  case FrameType::Open:
    if (!homeActive_) {
      LOG_WARN << "argus-tunnel: OPEN frame before authentication; ignored";
      break;
    }
    if (delegate_->validatesAuth()) {
      LOG_WARN << "argus-tunnel: unexpected OPEN frame from the client";
      break;
    }
    if (frame.streamId == 0 || streams_.contains(frame.streamId)) {
      LOG_WARN << "argus-tunnel: OPEN for stream " << frame.streamId
               << " which is invalid or already open; ignored";
      break;
    }
    if (static_cast<int>(streams_.size()) >= limits_.maxStreams) {
      const CloseReason busy = CloseReason::Busy;
      sendFrameToHome({.type = FrameType::Close,
                       .streamId = frame.streamId,
                       .payload = reinterpret_cast<const char*>(&busy),
                       .size = 1});
      break;
    }
    {
      Stream& stream = streams_[frame.streamId];
      stream.id = frame.streamId;
      stream.lastActivity = Clock::now();
      delegate_->onRemoteOpen(frame.streamId);
    }
    break;
  case FrameType::Data: {
    Stream* stream = findStream(frame.streamId);
    if (!stream)
      break;
    stream->lastActivity = Clock::now();
    const size_t size = frame.payload.size();
    if (!stream->local || !stream->pendingToLocal.empty() ||
        stream->local->congested()) {
      if (stream->pendingToLocal.size() + size >
          2 * limits_.streamPendingCap) {
        closeStream(frame.streamId, CloseReason::Backpressure);
        break;
      }
      stream->pendingToLocal.append(frame.payload);
      globalPendingToLocal_ += size;
      if (globalPendingToLocal_ >= limits_.linkHighWater && !homeReadPaused_ &&
          homePeer_) {
        homeReadPaused_ = true;
        homePeer_->setReadPaused(true);
      }
      break;
    }
    stream->local->send(frame.payload);
    break;
  }
  case FrameType::Close: {
    Stream* stream = findStream(frame.streamId);
    if (!stream)
      break;
    const CloseReason reason =
        frame.payload.empty()
            ? CloseReason::Normal
            : static_cast<CloseReason>(frame.payload[0]);
    closeLocal(*stream, reason);
    break;
  }
  case FrameType::Ping:
    sendFrameToHome({.type = FrameType::Pong,
                      .streamId = 0,
                      .payload = nullptr,
                      .size = 0});
    break;
  case FrameType::Pong:
    break;
  case FrameType::Push:
    if (!homeActive_)
      break;
    delegate_->onPushFrame(frame.payload);
    break;
  }
}

void TunnelMux::handleLocalRead(const LocalReadInput& input)
{
  Stream& stream = input.stream;
  if (!homeActive_ || !homePeer_ || homePeer_->closed()) {
    closeStream(stream.id, CloseReason::Error);
    return;
  }
  stream.lastActivity = Clock::now();
  stream.pendingToHome.append(input.data, input.size);
  globalPendingToHome_ += input.size;
  if (stream.pendingToHome.size() >= limits_.streamPendingCap &&
      !stream.localReadPaused && stream.local) {
    stream.localReadPaused = true;
    stream.local->setReadPaused(true);
  }
  if (globalPendingToHome_ >= limits_.globalPendingCap)
    pauseAllLocalReads();
  flushToHome(stream);
}

void TunnelMux::handleLocalEof(Stream& stream)
{
  stream.localEof = true;
  if (stream.pendingToHome.empty())
    closeStream(stream.id, CloseReason::Normal);
}

void TunnelMux::flushToHome(Stream& stream)
{
  if (!homePeer_ || homePeer_->closed())
    return;
  while (!stream.pendingToHome.empty() && !homePeer_->congested()) {
    const size_t chunk = std::min(stream.pendingToHome.size(), kDataFrameCap);
    sendFrameToHome({.type = FrameType::Data,
                     .streamId = stream.id,
                     .payload = stream.pendingToHome.data(),
                     .size = chunk});
    stream.pendingToHome.erase(0, chunk);
    globalPendingToHome_ -= chunk;
  }
  if (stream.localEof && stream.pendingToHome.empty()) {
    closeStream(stream.id, CloseReason::Normal);
    resumeLocalReads();
    return;
  }
  if (stream.pendingToHome.empty() && stream.localReadPaused &&
      globalPendingToHome_ < limits_.globalPendingCap / 2 && stream.local) {
    stream.localReadPaused = false;
    stream.local->setReadPaused(false);
  }
  resumeLocalReads();
}

void TunnelMux::flushToLocal(Stream& stream)
{
  if (!stream.local || stream.local->closed())
    return;
  while (!stream.pendingToLocal.empty() && !stream.local->congested()) {
    stream.local->send(stream.pendingToLocal);
    globalPendingToLocal_ -= stream.pendingToLocal.size();
    stream.pendingToLocal.clear();
  }
  if (stream.pendingToLocal.empty())
    resumeHomeRead();
}

void TunnelMux::pauseAllLocalReads()
{
  for (auto& [id, stream] : streams_) {
    if (stream.local && !stream.localReadPaused) {
      stream.localReadPaused = true;
      stream.local->setReadPaused(true);
    }
  }
}

void TunnelMux::resumeLocalReads()
{
  if (globalPendingToHome_ > limits_.globalPendingCap / 2)
    return;
  for (auto& [id, stream] : streams_) {
    if (stream.local && stream.localReadPaused &&
        stream.pendingToHome.empty()) {
      stream.localReadPaused = false;
      stream.local->setReadPaused(false);
    }
  }
}

void TunnelMux::resumeHomeRead()
{
  if (!homeReadPaused_ || !homePeer_)
    return;
  if (globalPendingToLocal_ <= limits_.linkHighWater / 2) {
    homeReadPaused_ = false;
    homePeer_->setReadPaused(false);
    pumpHomeFrames();
  }
}

void TunnelMux::sendFrameToHome(const FrameToHomeInput& input)
{
  if (!homePeer_ || homePeer_->closed())
    return;
  homePeer_->send(encodeFrame(
      {.type = input.type,
       .streamId = input.streamId,
       .payload = input.payload,
       .size = input.size}));
}

void TunnelMux::closeLocal(Stream& stream, CloseReason reason)
{
  const uint32_t id = stream.id;
  Stream moved = std::move(stream);
  streams_.erase(id);
  globalPendingToHome_ -= moved.pendingToHome.size();
  globalPendingToLocal_ -= moved.pendingToLocal.size();
  if (homeActive_)
    sendFrameToHome({.type = FrameType::Close,
                     .streamId = id,
                     .payload = reinterpret_cast<const char*>(&reason),
                     .size = 1});
  if (moved.local)
    releaseLocal(moved, reason);
  delegate_->onStreamClosed(id);
  resumeHomeRead();
}

void TunnelMux::releaseLocal(Stream& stream, CloseReason reason)
{
  const TcpPeer::Ptr local = stream.local;
  local->setCallbacks(TcpPeer::Callbacks{});
  if (reason != CloseReason::Normal || local->closed()) {
    local->close();
    return;
  }
  if (!stream.pendingToLocal.empty())
    local->send(stream.pendingToLocal);
  local->closeWhenFlushed();
  if (!local->closed())
    draining_.push_back({.peer = local, .deadline = Clock::now() + kDrainGrace});
}
}
