#pragma once

#include <net/fd.hxx>
#include <net/poll-loop.hxx>

#include <functional>
#include <memory>
#include <string>

// Accepting TCP socket on a PollLoop.
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

  // Returns nullptr when the bind fails.
  static std::unique_ptr<TcpListener> create(const Params& params);

  uint16_t boundPort() const;
  void handleEvents(uint32_t events) override;

private:
  explicit TcpListener(const Params& params);

  PollLoop& loop_;
  UniqueFd fd_;
  std::function<void(int fd, const std::string& peerIp, uint16_t peerPort)>
      onAccept_;
};
