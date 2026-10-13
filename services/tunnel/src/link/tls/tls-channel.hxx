#pragma once

#include <net/fd.hxx>
#include <net/poll-loop.hxx>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace tunnel::link
{
class TlsChannel : public LoopActor
{
public:
  using Ptr = std::shared_ptr<TlsChannel>;

  struct Callbacks
  {
    std::function<void(TlsChannel&)> onConnected;
    std::function<void(TlsChannel&, const char* data, size_t size)> onRead;
    std::function<void(TlsChannel&)> onEof;
    std::function<void(TlsChannel&)> onDrained;
    std::function<void(TlsChannel&)> onCongested;
    std::function<void(TlsChannel&)> onClosed;
  };

  struct Params
  {
    PollLoop* loop{nullptr};
    std::string host;
    uint16_t port{0};
    std::string serverName;
    SSL_CTX* context{nullptr};
    std::string pinnedSubjectPublicKeyDer;
    size_t sendLimit{static_cast<size_t>(256) * 1024};
    size_t sendHardCap{static_cast<size_t>(4) * 1024 * 1024};
    int sndBuf{0};
    Callbacks callbacks;
  };

  static Ptr connect(const Params& params);
  ~TlsChannel() override;
  TlsChannel(const TlsChannel&) = delete;
  TlsChannel& operator=(const TlsChannel&) = delete;

  void send(std::string_view data);
  void setCallbacks(Callbacks callbacks);
  void setReadPaused(bool paused);
  void closeWhenFlushed();
  void close();
  Ptr sharedFromThis()
  {
    return std::static_pointer_cast<TlsChannel>(shared_from_this());
  }
  [[nodiscard]] bool closed() const { return closed_; }
  [[nodiscard]] bool ready() const { return ready_; }
  [[nodiscard]] bool congested() const
  {
    return sendBuffer_.size() >= sendLimit_;
  }
  [[nodiscard]] size_t sendBufferBytes() const { return sendBuffer_.size(); }

  void handleEvents(uint32_t events) override;

private:
  explicit TlsChannel(const Params& params);

  static int openSocket(const Params& params);
  void dispatch(uint32_t events);
  void completeConnection();
  void stepHandshake();
  void verifyPeer();
  void readAvailable();
  void flush();
  void updateInterest();
  uint32_t interestEvents() const;
  void applyStaged();

  PollLoop& loop_;
  UniqueFd fd_;
  SSL* ssl_{nullptr};
  std::string host_;
  std::string serverName_;
  std::string pinnedKey_;
  size_t sendLimit_;
  size_t sendHardCap_;
  Callbacks callbacks_;
  int dispatchDepth_{0};
  bool staged_{false};
  Callbacks stagedCallbacks_;
  std::string sendBuffer_;
  bool connecting_{true};
  bool ready_{false};
  bool readPaused_{false};
  bool closeWhenFlushed_{false};
  bool sentShutdown_{false};
  bool congestedNotified_{false};
  bool wantWrite_{false};
  bool eofSeen_{false};
  bool closed_{false};
};
}
