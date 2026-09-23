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
class MuxDelegate
{
public:
  virtual ~MuxDelegate() = default;
  virtual const std::string& authSecret() const = 0;
  virtual bool validatesAuth() const = 0;
  virtual void onAuthAccepted() {}
  virtual void onAuthRejected() {}
  virtual void onPushFrame(const std::string& payload) { (void)payload; }
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

class TunnelMux
{
public:
  using Clock = std::chrono::steady_clock;

  struct Limits
  {
    size_t streamPendingCap{256 * 1024};
    size_t globalPendingCap{8 * 1024 * 1024};
    size_t linkHighWater{256 * 1024};
    int socketSndBuf{0};
    std::chrono::seconds idleTimeout{300};
    std::chrono::seconds deadLinkTimeout{90};
    std::chrono::seconds authTimeout{10};
    int maxStreams{256};
  };

  struct Deps
  {
    PollLoop& loop;
    Limits limits;
    MuxDelegate* delegate;
  };

  explicit TunnelMux(const Deps& deps);

  void adoptHome(const TcpPeer::Ptr& peer);
  void sendAuth();
  bool openLocal(uint32_t streamId, const TcpPeer::Ptr& peer);
  uint32_t openRemote();
  void closeStream(uint32_t streamId, CloseReason reason);
  void dropLink();
  void teardownAll();
  void sweep();
  void sendPing();
  bool sendPush(const std::string& payload);

  bool hasHome() const { return homePeer_ != nullptr; }
  bool homeActive() const { return homeActive_; }
  size_t streamCount() const { return streams_.size(); }
  size_t pendingBytes() const
  {
    return globalPendingToHome_ + globalPendingToLocal_;
  }

private:
  struct HomeReadInput
  {
    const char* data;
    size_t size{0};
  };

  struct LocalReadInput
  {
    Stream& stream;
    const char* data;
    size_t size{0};
  };

  struct FrameToHomeInput
  {
    FrameType type{};
    uint32_t streamId{0};
    const char* payload;
    size_t size{0};
  };

  Stream* findStream(uint32_t streamId);
  void handleHomeRead(const HomeReadInput& input);
  void handleHomeDrained(TcpPeer& peer);
  void dispatchFrame(Frame frame);
  void pumpHomeFrames();
  void handleLocalRead(const LocalReadInput& input);
  void handleLocalEof(Stream& stream);
  void flushToHome(Stream& stream);
  void flushToLocal(Stream& stream);
  void pauseAllLocalReads();
  void resumeLocalReads();
  void resumeHomeRead();
  void sendFrameToHome(const FrameToHomeInput& input);
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
}
