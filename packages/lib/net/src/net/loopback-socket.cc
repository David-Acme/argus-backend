#include "loopback-socket.hxx"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace argus::net
{

namespace
{

constexpr std::chrono::milliseconds kCancellationSlice{20};
constexpr std::size_t kReceiveChunk = 65536;
constexpr int kDefaultPort = 80;

Connection failed(Socket socket, int error)
{
  return {.socket = std::move(socket), .status = NetStatus::Failed, .error = error};
}

Connection cancelled()
{
  return {.socket = Socket{}, .status = NetStatus::Cancelled, .error = ECANCELED};
}

std::chrono::milliseconds pollSlice(std::chrono::milliseconds remaining,
                                    const std::stop_token& cancellation)
{
  if (!cancellation.stop_possible())
    return remaining;
  return std::min(remaining, kCancellationSlice);
}

int awaitWritable(int fd, const ConnectInput& input)
{
  pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
  const auto deadline = std::chrono::steady_clock::now() + input.timeout;
  for (;;) {
    if (input.cancellation.stop_requested())
      return ECANCELED;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
      return ETIMEDOUT;
    const auto slice = pollSlice(remaining, input.cancellation);
    const int ready = ::poll(&pfd, 1, static_cast<int>(slice.count()));
    const int pollError = errno;
    if (input.cancellation.stop_requested())
      return ECANCELED;
    if (ready > 0)
      return 0;
    if (ready < 0 && pollError != EINTR)
      return pollError;
  }
}

void applyStreamOptions(int fd, std::chrono::milliseconds timeout)
{
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);
  const timeval tv{.tv_sec = static_cast<time_t>(seconds.count()),
                   .tv_usec = static_cast<suseconds_t>(micros.count())};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

}

Socket::~Socket()
{
  if (fd_ >= 0)
    ::close(fd_);
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept
{
  if (this != &other) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

void Socket::shutdown() const noexcept
{
  if (fd_ >= 0)
    ::shutdown(fd_, SHUT_RDWR);
}

Endpoint parseEndpoint(std::string_view url)
{
  std::string_view rest = url;
  if (const auto scheme = rest.find("://"); scheme != std::string_view::npos)
    rest.remove_prefix(scheme + 3);
  if (const auto slash = rest.find('/'); slash != std::string_view::npos)
    rest = rest.substr(0, slash);
  const auto colon = rest.rfind(':');
  if (colon == std::string_view::npos)
    return {.host = std::string(rest), .port = kDefaultPort};
  return {.host = std::string(rest.substr(0, colon)),
          .port = std::stoi(std::string(rest.substr(colon + 1)))};
}

Connection connectLoopback(const ConnectInput& input)
{
  if (input.cancellation.stop_requested())
    return cancelled();
  Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!socket.valid())
    return failed(Socket{}, errno);
  const int fd = socket.get();
  const std::stop_callback wake(input.cancellation, [fd] { ::shutdown(fd, SHUT_RDWR); });

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(input.port));
  if (::inet_pton(AF_INET, std::string(input.host).c_str(), &address.sin_addr) != 1)
    return failed(Socket{}, EINVAL);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 &&
      errno != EINPROGRESS) {
    const int connectError = errno;
    if (input.cancellation.stop_requested())
      return cancelled();
    return failed(Socket{}, connectError);
  }
  if (const int waitError = awaitWritable(fd, input); waitError != 0) {
    if (waitError == ECANCELED)
      return cancelled();
    return failed(Socket{}, waitError);
  }
  int soError = 0;
  socklen_t length = sizeof(soError);
  const int checked = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
  if (input.cancellation.stop_requested())
    return cancelled();
  if (checked != 0)
    return failed(Socket{}, errno);
  if (soError != 0)
    return failed(Socket{}, soError);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  const auto blocking = static_cast<unsigned>(flags) & ~static_cast<unsigned>(O_NONBLOCK);
  if (flags < 0 || ::fcntl(fd, F_SETFL, static_cast<int>(blocking)) != 0)
    return failed(Socket{}, errno);
  applyStreamOptions(fd, input.timeout);
  if (input.cancellation.stop_requested())
    return cancelled();
  return {.socket = std::move(socket), .status = NetStatus::Ok, .error = 0};
}

NetStatus sendAll(const SendInput& input)
{
  std::size_t sent = 0;
  while (sent < input.data.size()) {
    if (input.cancellation.stop_requested())
      return NetStatus::Cancelled;
    const auto written = ::send(input.fd, input.data.data() + sent,
                                input.data.size() - sent, MSG_NOSIGNAL);
    const int error = errno;
    if (input.cancellation.stop_requested())
      return NetStatus::Cancelled;
    if (written < 0 && error == EINTR)
      continue;
    if (written <= 0)
      return NetStatus::Failed;
    sent += static_cast<std::size_t>(written);
  }
  return NetStatus::Ok;
}

NetStatus receiveSome(const ReceiveInput& input)
{
  const std::size_t offset = input.into.size();
  for (;;) {
    if (input.cancellation.stop_requested())
      return NetStatus::Cancelled;
    input.into.resize(offset + kReceiveChunk);
    const auto received = ::recv(input.fd, input.into.data() + offset, kReceiveChunk, 0);
    const int error = errno;
    input.into.resize(offset + static_cast<std::size_t>(std::max<ssize_t>(received, 0)));
    if (input.cancellation.stop_requested())
      return NetStatus::Cancelled;
    if (received < 0 && error == EINTR)
      continue;
    if (received == 0)
      return NetStatus::Closed;
    if (received < 0)
      return NetStatus::Failed;
    return NetStatus::Ok;
  }
}

ReadResult readUntilClosed(const ReadInput& input)
{
  ReadResult result;
  while (std::chrono::steady_clock::now() < input.deadline) {
    result.status = receiveSome(
        {.fd = input.fd, .into = result.data, .cancellation = input.cancellation});
    if (result.status != NetStatus::Ok)
      return result;
  }
  result.status = input.cancellation.stop_requested() ? NetStatus::Cancelled
                                                       : NetStatus::Failed;
  return result;
}

}
