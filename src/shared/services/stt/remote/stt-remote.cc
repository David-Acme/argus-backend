#include "stt-remote.hxx"

#include <shared/services/config-service/config-service.hxx>

#include <json/json.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr const char* kPcmMime = "audio/x-argus-pcm-s16";

// Inverse of the voice session's int16→float mapping (/32768): the wire
// carries the same quantization the session's WS frames use.
int16_t sampleFromFloat(float value)
{
  const float clamped = std::max(-1.0F, std::min(1.0F, value));
  const long scaled = std::lround(clamped * 32768.0F);
  return static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
}

void throwEnvelopeError(int status, const std::string& body)
{
  Json::Value json;
  Json::Reader reader;
  std::string detail = "HTTP " + std::to_string(status);
  if (reader.parse(body, json) && json.isObject() &&
      json["errors"].isObject()) {
    detail = json["errors"].get("code", "").asString() + ": " +
             json["errors"].get("message", "").asString();
  }
  throw std::runtime_error("argus-stt " + detail);
}

std::string pcmBytes(const std::vector<float>& audioSamples)
{
  std::string bytes;
  bytes.reserve(audioSamples.size() * sizeof(int16_t));
  for (const float sample : audioSamples) {
    const int16_t raw = sampleFromFloat(sample);
    bytes.append(reinterpret_cast<const char*>(&raw), sizeof(raw));
  }
  return bytes;
}

// Socket guard: the fd is closed on scope exit, never leaked.
class SocketGuard
{
public:
  explicit SocketGuard(int fd) : fd_(fd) {}
  ~SocketGuard()
  {
    if (fd_ >= 0)
      ::close(fd_);
  }
  SocketGuard(const SocketGuard&) = delete;
  SocketGuard& operator=(const SocketGuard&) = delete;
  int get() const { return fd_; }

private:
  int fd_;
};

int connectLoopback(const std::string& host, int port, int timeoutMs)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }

  // Non-blocking connect + poll: the timeout bounds the connect phase too
  // (SO_SNDTIMEO/SO_RCVTIMEO alone never do).
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
      errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLOUT;
  if (::poll(&pfd, 1, timeoutMs) != 1) {
    ::close(fd);
    return -1;
  }
  int soError = 0;
  socklen_t len = sizeof(soError);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) != 0 ||
      soError != 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFL, flags);

  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

struct Address
{
  std::string host;
  int port{0};
};

Address parseUrl(const std::string& url)
{
  Address address;
  std::string rest = url;
  const auto scheme = rest.find("://");
  if (scheme != std::string::npos)
    rest = rest.substr(scheme + 3);
  const auto slash = rest.find('/');
  if (slash != std::string::npos)
    rest = rest.substr(0, slash);
  const auto colon = rest.rfind(':');
  if (colon != std::string::npos) {
    address.host = rest.substr(0, colon);
    address.port = std::stoi(rest.substr(colon + 1));
  }
  else {
    address.host = rest;
    address.port = 80;
  }
  return address;
}

std::string requestHead(const SttWireRequest& request, const Address& address)
{
  std::string head = "POST " + request.path + " HTTP/1.1\r\n";
  head += "Host: " + address.host + "\r\n";
  head += "Content-Type: " + request.contentType + "\r\n";
  head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  head += "Connection: close\r\n\r\n";
  return head;
}

void sendAll(int fd, const std::string& data)
{
  size_t sent = 0;
  while (sent < data.size()) {
    const auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0)
      throw std::runtime_error("argus-stt request send failed");
    sent += static_cast<size_t>(n);
  }
}

// Reads until the peer closes or the deadline fires.
std::string readAll(int fd, const std::chrono::steady_clock::time_point& deadline)
{
  std::string data;
  char buffer[65536];
  while (std::chrono::steady_clock::now() < deadline) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      break;
    data.append(buffer, static_cast<size_t>(n));
  }
  return data;
}

struct Head
{
  int status{0};
  std::string::size_type bodyStart{0};
};

Head parseHead(const std::string& wire)
{
  Head head;
  const auto split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("argus-stt response has no header block");
  head.bodyStart = split + 4;

  const auto lineEnd = wire.find("\r\n");
  const std::string statusLine = wire.substr(0, lineEnd);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  if (space1 == std::string::npos || space2 == std::string::npos)
    throw std::runtime_error("argus-stt malformed status line");
  head.status = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));
  return head;
}

} // namespace

SttRemoteConfig SttRemoteConfig::resolve()
{
  SttRemoteConfig config;
  config.url = ConfigService::getString("stt.remote_url");
  if (const int ms = ConfigService::getInt("stt.remote_timeout_ms"); ms > 0)
    config.timeoutMs = ms;
  return config;
}

SttHttpClient::SttHttpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
  if (parseUrl(baseUrl_).host.empty())
    throw std::runtime_error("argus-stt remote_url has no host");
}

SttHttpClient::RawResponse SttHttpClient::exchange(
    const SttWireRequest& request) const
{
  const Address address = parseUrl(baseUrl_);
  const SocketGuard fd(connectLoopback(address.host, address.port, timeoutMs_));
  if (fd.get() < 0)
    throw std::runtime_error("argus-stt unreachable at " + baseUrl_);

  sendAll(fd.get(), requestHead(request, address) + request.body);

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);
  const std::string wire = readAll(fd.get(), deadline);
  if (wire.empty())
    throw std::runtime_error("argus-stt closed the connection before answering");

  const Head parsed = parseHead(wire);
  if (parsed.status != 200)
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  return {.status = parsed.status, .body = wire.substr(parsed.bodyStart)};
}

std::string SttHttpClient::transcribe(const std::vector<float>& audioSamples,
                                      const std::string& lang) const
{
  // The wire is 16 kHz mono by contract; any other rate cannot be
  // represented and must not be silently resampled.
  if (audioSamples.empty())
    throw std::runtime_error("argus-stt transcribe needs a non-empty body");

  SttWireRequest request;
  request.path = "/stt/v1/transcribe?lang=" + lang;
  request.body = pcmBytes(audioSamples);
  request.contentType = kPcmMime;

  const RawResponse response = exchange(request);
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("text"))
    throw std::runtime_error("argus-stt transcribe response unreadable");
  return json["info"]["text"].asString();
}
