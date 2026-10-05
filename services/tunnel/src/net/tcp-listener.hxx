#pragma once

#include <net/fd.hxx>
#include <net/poll-loop.hxx>

#include <functional>
#include <memory>
#include <string>

class TcpListener : public LoopActor
{
public:
  struct Params
  {
    PollLoop* loop{nullptr};
    std::string ip{"127.0.0.1"};
    uint16_t port{0};
    std::function<void(int fd, const std::string& peerIp, uint16_t peerPort)>
        onAccept;
  };

  static std::shared_ptr<TcpListener> create(const Params& params);

  uint16_t boundPort() const;
  void handleEvents(uint32_t events) override;
  void pauseAccepting();
  void resumeAccepting();
  bool acceptPaused() const { return acceptPaused_; }
  int acceptPauses() const { return acceptPauses_; }

  static constexpr int kAcceptBackoffMs = 200;

private:
  explicit TcpListener(const Params& params);

  PollLoop& loop_;
  UniqueFd fd_;
  std::function<void(int fd, const std::string& peerIp, uint16_t peerPort)>
      onAccept_;
  bool acceptPaused_{false};
  int acceptPauses_{0};
};
