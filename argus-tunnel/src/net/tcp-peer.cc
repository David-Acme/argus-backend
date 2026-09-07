#include "tcp-peer.hxx"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr size_t kReadChunk = 64 * 1024;
constexpr uint32_t kBaseEvents = EPOLLRDHUP;
}

TcpPeer::TcpPeer(const Params& params)
    : loop_(*params.loop),
      fd_(params.fd),
      callbacks_(params.callbacks),
      sendLimit_(params.sendLimit),
      sendHardCap_(params.sendHardCap),
      ip_(params.ip),
      port_(params.port)
{
}

TcpPeer::Ptr TcpPeer::adopt(const Params& params)
{
  if (params.fd < 0)
    throw std::runtime_error("TcpPeer::adopt requires a valid fd");
  if (params.sndBuf > 0) {
    int size = params.sndBuf;
    ::setsockopt(params.fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  }
  Ptr peer(new TcpPeer(params));
  peer->connected_ = true;
  peer->loop_.watch(peer->fd_.get(), peer.get());
  int one = 1;
  ::setsockopt(peer->fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return peer;
}

TcpPeer::Ptr TcpPeer::connect(const Params& params)
{
  Params updated = params;
  updated.fd = openSocket(params, true);
  Ptr peer(new TcpPeer(updated));
  peer->connecting_ = true;
  peer->loop_.watch(peer->fd_.get(), peer.get());
  peer->loop_.update(peer->fd_.get(), EPOLLOUT, peer.get());
  return peer;
}

int TcpPeer::openSocket(const Params& params, bool connecting)
{
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(params.port);
  if (::inet_pton(AF_INET, params.ip.c_str(), &address.sin_addr) != 1)
    throw std::runtime_error("invalid peer ip: " + params.ip);
  int fd = ::socket(AF_INET,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  if (params.sndBuf > 0) {
    int size = params.sndBuf;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  }
  if (connecting &&
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
          0 &&
      errno != EINPROGRESS) {
    ::close(fd);
    throw std::runtime_error("connect failed");
  }
  return fd;
}

TcpPeer::~TcpPeer()
{
  if (!closed_) {
    loop_.unwatch(fd_.get());
  }
}

void TcpPeer::send(const char* data, size_t size)
{
  if (closed_)
    return;
  sendBuffer_.append(data, size);
  flush();
  if (sendBuffer_.size() >= sendHardCap_) {
    close();
    return;
  }
  if (sendBuffer_.size() >= sendLimit_ && !congestedNotified_) {
    congestedNotified_ = true;
    if (callbacks_.onCongested)
      callbacks_.onCongested(*this);
  }
  updateInterest();
}

void TcpPeer::send(std::string data)
{
  send(data.data(), data.size());
}

void TcpPeer::setCallbacks(Callbacks callbacks)
{
  if (dispatchDepth_ > 0) {
    // Called from inside a callback: replacing callbacks_ would destroy the
    // executing std::function; apply the swap when the dispatch unwinds.
    stagedCallbacks_ = std::move(callbacks);
    staged_ = true;
    return;
  }
  callbacks_ = std::move(callbacks);
}

void TcpPeer::flush()
{
  if (closed_ || connecting_)
    return;
  while (!sendBuffer_.empty()) {
    ssize_t written = ::send(fd_.get(), sendBuffer_.data(),
                             sendBuffer_.size(), MSG_NOSIGNAL);
    if (written > 0) {
      sendBuffer_.erase(0, static_cast<size_t>(written));
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (written < 0 && errno == EINTR)
      continue;
    close();
    return;
  }
  if (sendBuffer_.empty() && congestedNotified_) {
    congestedNotified_ = false;
    if (callbacks_.onDrained)
      callbacks_.onDrained(*this);
  }
}

void TcpPeer::readAvailable()
{
  if (eofSeen_)
    return;
  char chunk[kReadChunk];
  while (true) {
    ssize_t received = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
    if (received > 0) {
      if (callbacks_.onRead)
        callbacks_.onRead(*this, chunk, static_cast<size_t>(received));
      if (closed_)
        return;
      continue;
    }
    if (received == 0) {
      eofSeen_ = true;
      if (callbacks_.onEof)
        callbacks_.onEof(*this);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    if (errno == EINTR)
      continue;
    close();
    return;
  }
}

void TcpPeer::handleConnectionComplete()
{
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
      error != 0) {
    closed_ = true;
    loop_.unwatch(fd_.get());
    fd_.reset();
    if (callbacks_.onClosed)
      callbacks_.onClosed(*this);
    return;
  }
  connecting_ = false;
  connected_ = true;
  int one = 1;
  ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  updateInterest();
  if (callbacks_.onConnected)
    callbacks_.onConnected(*this);
}

void TcpPeer::handleEvents(uint32_t events)
{
  ++dispatchDepth_;
  dispatch(events);
  --dispatchDepth_;
  applyStaged();
}

void TcpPeer::applyStaged()
{
  if (dispatchDepth_ > 0 || !staged_)
    return;
  staged_ = false;
  callbacks_ = std::move(stagedCallbacks_);
  stagedCallbacks_ = Callbacks{};
}

void TcpPeer::dispatch(uint32_t events)
{
  if (closed_)
    return;
  if (connecting_ && (events & EPOLLOUT)) {
    handleConnectionComplete();
    return;
  }
  if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
    readAvailable();
  if (closed_)
    return;
  if (events & (EPOLLOUT | EPOLLERR))
    flush();
  if ((events & EPOLLERR) && !closed_)
    close();
  else if (!closed_)
    updateInterest();
}

void TcpPeer::setReadPaused(bool paused)
{
  if (readPaused_ == paused || closed_)
    return;
  readPaused_ = paused;
  updateInterest();
}

void TcpPeer::close()
{
  if (closed_)
    return;
  closed_ = true;
  loop_.unwatch(fd_.get());
  fd_.reset();
  loop_.retain(shared_from_this());
  if (callbacks_.onClosed)
    callbacks_.onClosed(*this);
}

void TcpPeer::updateInterest()
{
  if (closed_)
    return;
  loop_.update(fd_.get(), interestEvents(), this);
}

uint32_t TcpPeer::interestEvents() const
{
  uint32_t events = kBaseEvents;
  if (!readPaused_)
    events |= EPOLLIN;
  if (!sendBuffer_.empty())
    events |= EPOLLOUT;
  return events;
}
