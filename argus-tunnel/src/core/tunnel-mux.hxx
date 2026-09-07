#pragma once

#include <net/tcp-peer.hxx>
#include <protocol/frames.hxx>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace tunnel
{
// Role hooks for the shared home-link multiplexer.
class MuxDelegate
{
public:
  virtual ~MuxDelegate() = default;
  virtual const std::string& authSecret() const = 0;
  // The relay verifies the peer's AUTH mac against the per-link challenge
  // and answers with its own AUTH_OK proof; the client verifies that proof
  // before activating the link.
  virtual bool validatesAuth() const = 0;
  virtual void onAuthAccepted() {}
  virtual void onAuthRejected() {}
  // The relay delivered a PUSH control frame; only dispatched after the home
  // link is authenticated (F5-5).
  virtual void onPushFrame(const std::string& payload) { (void)payload; }
  // The remote side opened a stream (client: dial the gateway).
  virtual void onRemoteOpen(uint32_t streamId) { (void)streamId; }
  virtual void onLinkUp() {}
  virtual void onLinkDown() {}
};

struct Stream
{
  uint32_t id{0};
  TcpPeer::Ptr local;
  std::string pendingToLocal;
  std::string pendingToHome;
  std::chrono::steady_clock::time_point lastActivity;
  bool localReadPaused{false};
};

// Shared stream multiplexer over the single home link: OPEN/DATA/CLOSE
// routing, bounded per-stream and global pending buffers with read
// pausing back-pressure, idle and dead-link sweeps.
class TunnelMux
{
public:
  using Clock = std::chrono::steady_clock;

  struct Limits
  {
    // Local reads pause at this pending depth and streams are killed past
    // twice it (the safety net for a stalled far end).
    size_t streamPendingCap{256 * 1024};
    size_t globalPendingCap{8 * 1024 * 1024};
    // Home-link read pauses when queued far-end bytes reach this mark.
    size_t linkHighWater{256 * 1024};
    // SO_SNDBUF applied to every tunnel socket (0 = kernel default); keeps
    // back-pressure in software queues instead of kernel buffers.
    int socketSndBuf{0};
    std::chrono::seconds idleTimeout{300};
    std::chrono::seconds deadLinkTimeout{90};
    std::chrono::seconds authTimeout{10};
    int maxStreams{256};
  };

  TunnelMux(PollLoop& loop, Limits limits, MuxDelegate* delegate);

  // Takes ownership of the home-link socket (accepted on the relay,
  // connected on the client).
  void adoptHome(const TcpPeer::Ptr& peer);
  // Client side: sends the AUTH mac bound to the current link challenge.
  void sendAuth();
  // Registers a locally-created socket for a stream (device connection on
  // the relay, dialed gateway connection on the client).
  bool openLocal(uint32_t streamId, const TcpPeer::Ptr& peer);
  // Relay side: allocates a stream id, registers it and sends OPEN.
  // Returns 0 when the stream cap is reached or the link is down.
  uint32_t openRemote();
  void closeStream(uint32_t streamId, CloseReason reason);
  void dropLink();
  void teardownAll();
  void sweep();
  void sendPing();
  // Relay side: sends a PUSH control frame toward the home client (F5-5);
  // loop thread only.
  bool sendPush(const std::string& payload);

  bool hasHome() const { return homePeer_ != nullptr; }
  bool homeActive() const { return homeActive_; }
  size_t streamCount() const { return streams_.size(); }
  size_t pendingBytes() const
  {
    return globalPendingToHome_ + globalPendingToLocal_;
  }

private:
  Stream* findStream(uint32_t streamId);
  void handleHomeRead(TcpPeer& peer, const char* data, size_t size);
  void handleHomeEof(TcpPeer& peer);
  void handleHomeDrained(TcpPeer& peer);
  void dispatchFrame(Frame frame);
  void pumpHomeFrames();
  void handleLocalRead(Stream& stream, const char* data, size_t size);
  void handleLocalEof(Stream& stream);
  void flushToHome(Stream& stream);
  void flushToLocal(Stream& stream);
  void pauseAllLocalReads();
  void resumeLocalReads();
  void resumeHomeRead();
  void sendFrameToHome(FrameType type, uint32_t streamId, const char* payload,
                       size_t size);
  void closeLocal(Stream& stream, CloseReason reason);

  PollLoop& loop_;
  Limits limits_;
  MuxDelegate* delegate_;
  TcpPeer::Ptr homePeer_;
  FrameParser parser_;
  std::string authChallenge_;
  std::unordered_map<uint32_t, Stream> streams_;
  size_t globalPendingToHome_{0};
  size_t globalPendingToLocal_{0};
  uint32_t nextStreamId_{1};
  bool homeActive_{false};
  bool homeReadPaused_{false};
  Clock::time_point lastFrameAt_;
  Clock::time_point attachedAt_;
};
} // namespace tunnel
