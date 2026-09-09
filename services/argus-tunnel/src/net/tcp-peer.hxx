#pragma once

#include <net/fd.hxx>
#include <net/poll-loop.hxx>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Non-blocking stream socket on a PollLoop: bounded send buffer, pausable reads, congestion notifications.
class TcpPeer : public LoopActor
{
public:
  using Ptr = std::shared_ptr<TcpPeer>;

  struct Callbacks
  {
    std::function<void(TcpPeer&)> onConnected;
    std::function<void(TcpPeer&, const char* data, size_t size)> onRead;
    std::function<void(TcpPeer&)> onEof;
    std::function<void(TcpPeer&)> onCongested;
    std::function<void(TcpPeer&)> onDrained;
    std::function<void(TcpPeer&)> onClosed;
  };

  struct Params
  {
    PollLoop* loop{nullptr};
    int fd{-1};
    std::string ip;
    uint16_t port{0};
    size_t sendLimit{256 * 1024};
    size_t sendHardCap{4 * 1024 * 1024};
    int sndBuf{0};
    Callbacks callbacks;
  };

  // Adopts an accepted socket; the fd becomes owned by the peer.
  static Ptr adopt(const Params& params);
  // Starts a non-blocking connect; onConnected fires on success, onClosed on failure.
  static Ptr connect(const Params& params);

  ~TcpPeer() override;

  void send(const char* data, size_t size);
  void send(std::string data);
  Ptr sharedFromThis()
  {
    return std::static_pointer_cast<TcpPeer>(shared_from_this());
  }
  void setCallbacks(Callbacks callbacks);
  void setReadPaused(bool paused);
  void close();
  bool closed() const { return closed_; }
  bool connected() const { return connected_ && !closed_; }
  bool congested() const { return sendBuffer_.size() >= sendLimit_; }
  size_t sendBufferBytes() const { return sendBuffer_.size(); }
  const std::string& peerIp() const { return ip_; }

  void handleEvents(uint32_t events) override;

private:
  explicit TcpPeer(const Params& params);
  static int openSocket(const Params& params, bool connecting);
  void dispatch(uint32_t events);
  void flush();
  void readAvailable();
  void handleConnectionComplete();
  void updateInterest();
  uint32_t interestEvents() const;
  void applyStaged();

  PollLoop& loop_;
  UniqueFd fd_;
  Callbacks callbacks_;
  int dispatchDepth_{0};
  bool staged_{false};
  Callbacks stagedCallbacks_;
  std::string sendBuffer_;
  size_t sendLimit_;
  size_t sendHardCap_;
  std::string ip_;
  uint16_t port_;
  bool readPaused_{false};
  bool connecting_{false};
  bool connected_{false};
  bool congestedNotified_{false};
  bool eofSeen_{false};
  bool closed_{false};
};
