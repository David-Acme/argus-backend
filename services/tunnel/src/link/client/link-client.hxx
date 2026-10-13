#pragma once

#include <core/push-queue.hxx>
#include <link/protocol/credit-window.hxx>
#include <link/protocol/frames.hxx>
#include <link/tls/client-tls-context.hxx>
#include <link/tls/host-resolver.hxx>
#include <link/tls/tls-channel.hxx>
#include <net/poll-loop.hxx>
#include <net/tcp-peer.hxx>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tunnel::link
{
struct LinkLimits
{
  size_t streamPendingCap{static_cast<size_t>(256) * 1024};
  size_t globalPendingCap{static_cast<size_t>(8) * 1024 * 1024};
  size_t linkHighWater{static_cast<size_t>(256) * 1024};
  int socketSndBuf{0};
  size_t maxStreams{256};
  std::chrono::seconds streamIdleTimeout{300};
};

struct LinkBackoffInput
{
  int attempt{1};
  int baseMs{1000};
  int capMs{60000};
  double jitter{0.0};
};

int linkReconnectDelayMs(const LinkBackoffInput& input);

struct LinkOptions
{
  std::string host{"argus-tunnel.lynk.page"};
  uint16_t port{7001};
  std::string gatewayHost{"127.0.0.1"};
  uint16_t gatewayPort{7142};
  std::string certificatePath;
  std::string keyPath;
  std::string pinnedSubjectPublicKeyDer;
  LinkLimits limits;
  std::chrono::seconds keepaliveActive{20};
  std::chrono::seconds keepaliveIdle{90};
  std::chrono::seconds keepalivePongTimeout{15};
  std::chrono::seconds deadLinkTimeout{240};
  std::chrono::seconds helloTimeout{10};
  int reconnectBaseMs{1000};
  int reconnectCapMs{60000};
  uint32_t capabilities{capability::kPushIntents | capability::kStreamCredit};
  size_t pushQueueCapacity{256};
};

class LinkObserver
{
public:
  virtual ~LinkObserver() = default;
  virtual void onLinkUp() {}
  virtual void onLinkDown() {}
  virtual void onGoingAway(GoAwayReason reason, uint16_t seconds)
  {
    (void)reason;
    (void)seconds;
  }
  virtual void onPushIntent(std::string_view intent) { (void)intent; }
  virtual void onWireError(WireError error) { (void)error; }
};

class LinkClient
{
public:
  using Clock = std::chrono::steady_clock;

  struct Deps
  {
    PollLoop& loop;
    LinkOptions options;
    LinkObserver* observer{nullptr};
  };

  explicit LinkClient(const Deps& deps);
  ~LinkClient();
  LinkClient(const LinkClient&) = delete;
  LinkClient& operator=(const LinkClient&) = delete;

  void start();
  void stop();

  [[nodiscard]] bool connected() const { return connected_.load(); }
  [[nodiscard]] bool draining() const { return draining_.load(); }
  [[nodiscard]] bool keepaliveActive() const { return activeKeepalive_.load(); }
  [[nodiscard]] size_t streamCount() const { return streamCount_.load(); }
  [[nodiscard]] int reconnectAttempts() const { return attempts_.load(); }
  [[nodiscard]] int lastReconnectDelayMs() const { return lastDelayMs_.load(); }
  [[nodiscard]] uint32_t remoteCapabilities() const
  {
    return remoteCapabilities_.load();
  }
  [[nodiscard]] size_t pendingBytes() const { return pendingBytes_.load(); }

  [[nodiscard]] size_t pushQueued() const { return pushQueue_.size(); }
  [[nodiscard]] uint64_t pushReceived() const { return pushQueue_.received(); }
  [[nodiscard]] uint64_t pushDropped() const { return pushQueue_.dropped(); }

private:
  struct Stream
  {
    uint32_t id{0};
    TcpPeer::Ptr local;
    std::string pendingToLocal;
    std::string pendingToHome;
    CreditWindow sendWindow;
    CreditWindow receiveWindow;
    Clock::time_point lastActivity;
    bool localAttached{false};
    bool localReadPaused{false};
    bool localEof{false};
    bool closedByNode{false};
  };

  struct DrainingPeer
  {
    TcpPeer::Ptr peer;
    Clock::time_point deadline;
  };

  struct FrameToNodeInput
  {
    std::string frame;
  };

  void connectHome();
  void openChannel(const std::vector<Endpoint>& endpoints);
  void installChannelCallbacks();
  void closeLink(std::string_view reason);
  void refuseLink(WireError error);
  void handleLinkRead(const char* data, size_t size);
  void dispatchFrame(const Frame& frame);
  void pumpFrames();
  void handleHello(const Frame& frame);
  void handleOpen(const Frame& frame);
  void handleData(const Frame& frame);
  void handleClose(const Frame& frame);
  void handlePing(const Frame& frame);
  void handlePong(const Frame& frame);
  void handlePush(const Frame& frame);
  void handleGoAway(const Frame& frame);
  void handleCredit(const Frame& frame);

  Stream* findStream(uint32_t streamId);
  void openStream(uint32_t streamId);
  void writeToStream(uint32_t streamId, const char* data, size_t size);
  void handleLocalEof(uint32_t streamId);
  void flushToHome(uint32_t streamId);
  void flushToLocal(uint32_t streamId);
  void closeStream(uint32_t streamId, CloseReason reason);
  void closeLocal(Stream& stream, CloseReason reason);
  void releaseLocal(Stream& stream, CloseReason reason);
  void teardownStreams();
  void grantConsumedCredit(Stream& stream, size_t consumedBytes);
  void pauseAllLocalReads();
  void resumeLocalReads();
  void resumeLinkRead();
  void sendFrameToNode(const FrameToNodeInput& input);
  void updatePending();

  void scheduleSweep();
  void scheduleReconnect();
  void armReconnect(int delayMs);
  int nextDelayMs();
  void sweep();
  void keepaliveTick();

  PollLoop& loop_;
  LinkOptions options_;
  LinkObserver* observer_;
  ClientTlsContext tlsContext_;
  TlsChannel::Ptr channel_;
  FrameParser parser_;
  std::unordered_map<uint32_t, Stream> streams_;
  std::vector<DrainingPeer> drainingPeers_;
  size_t globalPendingToHome_{0};
  size_t globalPendingToLocal_{0};
  uint32_t localCapabilities_{0};
  bool negotiatedCredit_{false};
  bool helloReceived_{false};
  bool linkReadPaused_{false};
  bool awaitingPong_{false};
  bool reconnectWithoutBackoff_{false};
  uint64_t pingNonce_{0};
  Clock::time_point attachedAt_;
  Clock::time_point lastFrameAt_;
  Clock::time_point lastPingAt_;
  std::mt19937_64 generator_;
  std::jthread resolver_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::weak_ptr<bool> aliveToken_{alive_};
  PushQueue pushQueue_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> activeKeepalive_{false};
  std::atomic<size_t> streamCount_{0};
  std::atomic<size_t> pendingBytes_{0};
  std::atomic<int> attempts_{0};
  std::atomic<int> lastDelayMs_{0};
  std::atomic<uint32_t> remoteCapabilities_{0};
  bool reconnectPending_{false};
};
}
