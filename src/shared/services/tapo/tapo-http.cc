#include "tapo-http.hxx"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr size_t kReadChunk = 8192;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 8 * 1024 * 1024;

std::string lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool waitReady(int fd, short events, int timeoutMs)
{
  pollfd descriptor{};
  descriptor.fd = fd;
  descriptor.events = events;
  for (;;) {
    const int ready = ::poll(&descriptor, 1, timeoutMs);
    if (ready > 0)
      return true;
    if (ready == 0)
      return false;
    if (errno != EINTR)
      return false;
  }
}

} // namespace

struct TapoConnection::Impl
{
  int fd{-1};
  std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)> context{nullptr, &SSL_CTX_free};
  std::unique_ptr<SSL, void (*)(SSL*)> ssl{nullptr, &SSL_free};
  int ioTimeoutMs{5000};
  std::string buffer;
  size_t cursor{0};
  std::string error;

  ~Impl() { reset(); }

  void reset()
  {
    ssl.reset();
    context.reset();
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    buffer.clear();
    cursor = 0;
  }

  bool fill()
  {
    if (cursor > 0 && cursor == buffer.size()) {
      buffer.clear();
      cursor = 0;
    }
    char chunk[kReadChunk];
    int received = 0;
    for (;;) {
      if (ssl) {
        received = SSL_read(ssl.get(), chunk, static_cast<int>(sizeof(chunk)));
        if (received > 0)
          break;
        const int reason = SSL_get_error(ssl.get(), received);
        if (reason == SSL_ERROR_ZERO_RETURN) {
          error = "connection closed";
          return false;
        }
        if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
          const short events = reason == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
          if (!waitReady(fd, events, ioTimeoutMs)) {
            error = "read timeout";
            return false;
          }
          continue;
        }
        error = "tls read failed";
        return false;
      }
      received = static_cast<int>(::recv(fd, chunk, sizeof(chunk), 0));
      if (received > 0)
        break;
      if (received == 0) {
        error = "connection closed";
        return false;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!waitReady(fd, POLLIN, ioTimeoutMs)) {
          error = "read timeout";
          return false;
        }
        continue;
      }
      error = std::strerror(errno);
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(received));
    return true;
  }
};

TapoConnection::TapoConnection() : impl_(std::make_unique<Impl>()) {}

TapoConnection::~TapoConnection() = default;

bool TapoConnection::open(const TapoEndpoint& endpoint)
{
  close();
  impl_->ioTimeoutMs = endpoint.ioTimeoutMs;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  const std::string port = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &resolved) !=
      0) {
    impl_->error = "cannot resolve " + endpoint.host;
    return false;
  }

  int fd = -1;
  for (addrinfo* candidate = resolved; candidate;
       candidate = candidate->ai_next) {
    fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                  candidate->ai_protocol);
    if (fd < 0)
      continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
      break;
    if (errno == EINPROGRESS &&
        waitReady(fd, POLLOUT, endpoint.connectTimeoutMs)) {
      int status = 0;
      socklen_t length = sizeof(status);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &length) == 0 &&
          status == 0)
        break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(resolved);

  if (fd < 0) {
    impl_->error = "cannot connect to " + endpoint.host + ":" + port;
    return false;
  }
  impl_->fd = fd;

  if (!endpoint.tls)
    return true;

  impl_->context.reset(SSL_CTX_new(TLS_client_method()));
  if (!impl_->context) {
    impl_->error = "cannot create TLS context";
    close();
    return false;
  }
  SSL_CTX_set_verify(impl_->context.get(), SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_options(impl_->context.get(), SSL_OP_ALL);
  SSL_CTX_set_security_level(impl_->context.get(), 0);
  SSL_CTX_set_min_proto_version(impl_->context.get(), TLS1_VERSION);
  SSL_CTX_set_cipher_list(impl_->context.get(), "ALL:@SECLEVEL=0");

  impl_->ssl.reset(SSL_new(impl_->context.get()));
  if (!impl_->ssl) {
    impl_->error = "cannot create TLS session";
    close();
    return false;
  }
  SSL_set_fd(impl_->ssl.get(), fd);
  SSL_set_tlsext_host_name(impl_->ssl.get(), endpoint.host.c_str());

  for (;;) {
    const int handshake = SSL_connect(impl_->ssl.get());
    if (handshake == 1)
      break;
    const int reason = SSL_get_error(impl_->ssl.get(), handshake);
    if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
      const short events = reason == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      if (waitReady(fd, events, endpoint.connectTimeoutMs))
        continue;
      impl_->error = "TLS handshake timeout";
      close();
      return false;
    }
    impl_->error = "TLS handshake failed";
    close();
    return false;
  }
  return true;
}

void TapoConnection::close()
{
  impl_->reset();
}

bool TapoConnection::isOpen() const
{
  return impl_->fd >= 0;
}

bool TapoConnection::write(const std::string& data)
{
  size_t sent = 0;
  while (sent < data.size()) {
    int written = 0;
    if (impl_->ssl) {
      written = SSL_write(impl_->ssl.get(), data.data() + sent,
                          static_cast<int>(data.size() - sent));
      if (written <= 0) {
        const int reason = SSL_get_error(impl_->ssl.get(), written);
        if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
          const short events = reason == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
          if (!waitReady(impl_->fd, events, impl_->ioTimeoutMs)) {
            impl_->error = "write timeout";
            return false;
          }
          continue;
        }
        impl_->error = "tls write failed";
        return false;
      }
    }
    else {
      written = static_cast<int>(::send(impl_->fd, data.data() + sent,
                                        data.size() - sent, MSG_NOSIGNAL));
      if (written <= 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (!waitReady(impl_->fd, POLLOUT, impl_->ioTimeoutMs)) {
            impl_->error = "write timeout";
            return false;
          }
          continue;
        }
        impl_->error = std::strerror(errno);
        return false;
      }
    }
    sent += static_cast<size_t>(written);
  }
  return true;
}

bool TapoConnection::readLine(std::string& line)
{
  for (;;) {
    const size_t newline = impl_->buffer.find('\n', impl_->cursor);
    if (newline != std::string::npos) {
      line = impl_->buffer.substr(impl_->cursor, newline - impl_->cursor);
      impl_->cursor = newline + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      return true;
    }
    if (impl_->buffer.size() - impl_->cursor > kMaxHeaderBytes) {
      impl_->error = "header too large";
      return false;
    }
    if (!impl_->fill())
      return false;
  }
}

bool TapoConnection::readExactly(size_t count, std::string& out)
{
  if (count > kMaxBodyBytes) {
    impl_->error = "body too large";
    return false;
  }
  while (impl_->buffer.size() - impl_->cursor < count) {
    if (!impl_->fill())
      return false;
  }
  out = impl_->buffer.substr(impl_->cursor, count);
  impl_->cursor += count;
  return true;
}

bool TapoConnection::readSome(std::string& out)
{
  if (impl_->cursor >= impl_->buffer.size() && !impl_->fill())
    return false;
  out = impl_->buffer.substr(impl_->cursor);
  impl_->cursor = impl_->buffer.size();
  return true;
}

const std::string& TapoConnection::error() const
{
  return impl_->error;
}

std::string TapoHttpResponse::header(const std::string& name) const
{
  const std::string needle = lower(name);
  for (const auto& entry : headers) {
    if (lower(entry.name) == needle)
      return entry.value;
  }
  return {};
}

std::string TapoHttp::buildRequestHead(const TapoHttpRequest& request)
{
  std::string head = request.method + " " + request.path + " HTTP/1.1\r\n";
  head += "Host: " + request.endpoint.host + ":" +
          std::to_string(request.endpoint.port) + "\r\n";
  for (const auto& entry : request.headers)
    head += entry.name + ": " + entry.value + "\r\n";
  head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  head += "\r\n";
  return head;
}

bool TapoHttp::readResponseHead(TapoConnection& connection,
                                TapoHttpResponse& response)
{
  std::string line;
  if (!connection.readLine(line)) {
    response.error = connection.error();
    return false;
  }
  const size_t firstSpace = line.find(' ');
  if (firstSpace == std::string::npos) {
    response.error = "malformed status line";
    return false;
  }
  const size_t secondSpace = line.find(' ', firstSpace + 1);
  response.status = std::atoi(
      line.substr(firstSpace + 1, secondSpace - firstSpace - 1).c_str());

  for (;;) {
    if (!connection.readLine(line)) {
      response.error = connection.error();
      return false;
    }
    if (line.empty())
      break;
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string value = line.substr(colon + 1);
    const size_t begin = value.find_first_not_of(" \t");
    response.headers.push_back(
        {line.substr(0, colon),
         begin == std::string::npos ? "" : value.substr(begin)});
  }
  return true;
}

bool TapoHttp::readResponseBody(TapoConnection& connection,
                                TapoHttpResponse& response)
{
  const std::string encoding = lower(response.header("Transfer-Encoding"));
  if (encoding.find("chunked") != std::string::npos) {
    for (;;) {
      std::string line;
      if (!connection.readLine(line)) {
        response.error = connection.error();
        return false;
      }
      const size_t size = std::strtoul(line.c_str(), nullptr, 16);
      if (size == 0) {
        connection.readLine(line);
        return true;
      }
      std::string chunk;
      if (!connection.readExactly(size, chunk)) {
        response.error = connection.error();
        return false;
      }
      response.body += chunk;
      connection.readLine(line);
    }
  }

  const std::string length = response.header("Content-Length");
  if (!length.empty()) {
    const size_t size = std::strtoul(length.c_str(), nullptr, 10);
    if (size == 0)
      return true;
    if (!connection.readExactly(size, response.body)) {
      response.error = connection.error();
      return false;
    }
    return true;
  }

  std::string chunk;
  while (connection.readSome(chunk))
    response.body += chunk;
  return true;
}

TapoHttpResponse TapoHttp::send(const TapoHttpRequest& request)
{
  TapoHttpResponse response;
  TapoConnection connection;
  if (!connection.open(request.endpoint)) {
    response.error = connection.error();
    return response;
  }
  if (!connection.write(buildRequestHead(request) + request.body)) {
    response.error = connection.error();
    return response;
  }
  if (!readResponseHead(connection, response))
    return response;
  if (!readResponseBody(connection, response))
    return response;
  response.ok = true;
  return response;
}
