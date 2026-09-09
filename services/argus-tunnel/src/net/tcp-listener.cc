#include "tcp-listener.hxx"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

TcpListener::TcpListener(const Params& params)
    : loop_(*params.loop), onAccept_(params.onAccept)
{
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(params.port);
  if (::inet_pton(AF_INET, params.ip.c_str(), &address.sin_addr) != 1)
    throw std::runtime_error("invalid listener ip: " + params.ip);
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  fd_ = UniqueFd(fd);
  int one = 1;
  ::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(fd_.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0)
    throw std::runtime_error("bind failed");
  if (::listen(fd_.get(), 64) < 0)
    throw std::runtime_error("listen failed");
  loop_.watch(fd_.get(), this);
}

std::unique_ptr<TcpListener> TcpListener::create(const Params& params)
{
  try {
    return std::unique_ptr<TcpListener>(new TcpListener(params));
  } catch (const std::exception&) {
    return nullptr;
  }
}

uint16_t TcpListener::boundPort() const
{
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&address),
                    &length) < 0)
    return 0;
  return ntohs(address.sin_port);
}

void TcpListener::handleEvents(uint32_t events)
{
  if (!(events & EPOLLIN))
    return;
  while (true) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    int fd = ::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (errno == EINTR)
        continue;
      return;
    }
    char ip[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    if (onAccept_)
      onAccept_(fd, ip, ntohs(peer.sin_port));
    else
      ::close(fd);
  }
}
