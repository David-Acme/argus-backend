#include "tts-remote.hxx"

#include <shared/services/config-service/config-service.hxx>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <json/json.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace
{

std::string toLower(std::string value)
{
  for (auto& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string trim(const std::string& value)
{
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
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
  throw std::runtime_error("argus-tts " + detail);
}

std::vector<float> pcmFromBytes(const std::string& bytes)
{
  if (bytes.size() % sizeof(float) != 0)
    throw std::runtime_error("argus-tts PCM body is not float32-aligned");
  std::vector<float> pcm(bytes.size() / sizeof(float));
  std::memcpy(pcm.data(), bytes.data(), bytes.size());
  return pcm;
}

std::string jsonBody(const TtsRequest& req)
{
  Json::Value json(Json::objectValue);
  json["text"] = req.text;
  json["style_id"] = req.voiceId;
  json["lang"] = langCode(req.lang);
  if (req.speed > 0)
    json["speed"] = static_cast<double>(req.speed);
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, json);
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

std::string requestHead(const WireRequest& request, const Address& address)
{
  std::string head = request.method + " " + request.path + " HTTP/1.1\r\n";
  head += "Host: " + address.host + "\r\n";
  if (!request.body.empty()) {
    head += "Content-Type: application/json\r\n";
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  // The stream leg must keep the connection open: Drogon refuses to chunk
  // a Connection-close response.
  if (request.closeConnection)
    head += "Connection: close\r\n";
  head += "\r\n";
  return head;
}

void sendAll(int fd, const std::string& data)
{
  size_t sent = 0;
  while (sent < data.size()) {
    const auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0)
      throw std::runtime_error("argus-tts request send failed");
    sent += static_cast<size_t>(n);
  }
}

// Reads until the peer closes or the recv timeout fires; returns everything
// received. `deadline` bounds the whole exchange, not a single recv.
std::string readAll(int fd, const std::chrono::steady_clock::time_point& deadline)
{
  std::string data;
  char buffer[16384];
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
  bool chunked{false};
  size_t contentLength{0};
  std::string::size_type bodyStart{0};
};

Head parseHead(const std::string& wire)
{
  Head head;
  const auto split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    throw std::runtime_error("argus-tts response has no header block");
  head.bodyStart = split + 4;

  const auto lineEnd = wire.find("\r\n");
  const std::string statusLine = wire.substr(0, lineEnd);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  if (space1 == std::string::npos || space2 == std::string::npos)
    throw std::runtime_error("argus-tts malformed status line");
  head.status = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));

  std::string::size_type cursor = lineEnd + 2;
  while (cursor < split) {
    const auto eol = wire.find("\r\n", cursor);
    if (eol == std::string::npos || eol > split)
      break;
    const auto colon = wire.find(':', cursor);
    if (colon != std::string::npos && colon < eol) {
      const std::string name = toLower(wire.substr(cursor, colon - cursor));
      const std::string value = trim(wire.substr(colon + 1, eol - colon - 1));
      if (name == "transfer-encoding" &&
          toLower(value).find("chunked") != std::string::npos)
        head.chunked = true;
      if (name == "content-length")
        head.contentLength = static_cast<size_t>(std::stoul(value));
    }
    cursor = eol + 2;
  }
  return head;
}

// De-chunks a buffered chunked body; calls onChunk for every HTTP chunk.
void forEachHttpChunk(const std::string& wire, const Head& head,
                      const std::function<void(const char*, size_t)>& onChunk)
{
  std::string::size_type cursor = head.bodyStart;
  for (;;) {
    const auto lineEnd = wire.find("\r\n", cursor);
    if (lineEnd == std::string::npos)
      throw std::runtime_error("argus-tts chunked body truncated");
    const std::string sizeLine = wire.substr(cursor, lineEnd - cursor);
    const auto semicolon = sizeLine.find(';');
    const std::string sizeHex =
        trim(semicolon == std::string::npos
                 ? sizeLine
                 : sizeLine.substr(0, semicolon));
    if (sizeHex.empty())
      throw std::runtime_error("argus-tts malformed chunk size");
    const size_t size =
        static_cast<size_t>(std::stoul(sizeHex, nullptr, 16));
    if (size == 0)
      return;
    const auto dataStart = lineEnd + 2;
    if (wire.size() < dataStart + size + 2)
      throw std::runtime_error("argus-tts chunked body truncated");
    onChunk(wire.data() + dataStart, size);
    cursor = dataStart + size + 2;
  }
}

} // namespace

TtsRemoteConfig TtsRemoteConfig::resolve()
{
  TtsRemoteConfig config;
  config.url = ConfigService::getString("tts.remote_url");
  if (const int ms = ConfigService::getInt("tts.remote_timeout_ms"); ms > 0)
    config.timeoutMs = ms;
  return config;
}

TtsHttpClient::TtsHttpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs)
{
  if (parseUrl(baseUrl_).host.empty())
    throw std::runtime_error("argus-tts remote_url has no host");
}

TtsHttpClient::RawResponse TtsHttpClient::exchange(
    const WireRequest& request) const
{
  const Address address = parseUrl(baseUrl_);
  const SocketGuard fd(connectLoopback(address.host, address.port, timeoutMs_));
  if (fd.get() < 0)
    throw std::runtime_error("argus-tts unreachable at " + baseUrl_);

  const std::string head = requestHead(request, address);
  sendAll(fd.get(), head + request.body);

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);
  const std::string wire = readAll(fd.get(), deadline);
  if (wire.empty())
    throw std::runtime_error("argus-tts closed the connection before answering");

  const Head parsed = parseHead(wire);
  if (parsed.status != 200)
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  if (parsed.chunked) {
    std::string body;
    forEachHttpChunk(wire, parsed,
                     [&body](const char* data, size_t size) {
                       body.append(data, size);
                     });
    return {.status = parsed.status, .body = std::move(body)};
  }
  return {.status = parsed.status,
          .body = wire.substr(parsed.bodyStart, parsed.contentLength)};
}

void TtsHttpClient::stream(
    const WireRequest& request,
    const std::function<void(const char*, size_t)>& onChunk) const
{
  const Address address = parseUrl(baseUrl_);
  const SocketGuard fd(connectLoopback(address.host, address.port, timeoutMs_));
  if (fd.get() < 0)
    throw std::runtime_error("argus-tts unreachable at " + baseUrl_);

  const std::string head = requestHead(request, address);
  sendAll(fd.get(), head + request.body);

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs_);

  // The connection stays open (Drogon refuses to chunk a Connection-close
  // response), so the chunked body is de-chunked incrementally and the
  // terminal zero chunk ends the read.
  std::string wire;
  auto recvMore = [&]() -> bool {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    char buffer[16384];
    const auto n = ::recv(fd.get(), buffer, sizeof(buffer), 0);
    if (n <= 0)
      return false;
    wire.append(buffer, static_cast<size_t>(n));
    return true;
  };

  while (wire.find("\r\n\r\n") == std::string::npos) {
    if (!recvMore())
      throw std::runtime_error("argus-tts response has no header block");
  }
  const Head parsed = parseHead(wire);
  if (parsed.status != 200) {
    while (wire.size() < parsed.bodyStart + parsed.contentLength) {
      if (!recvMore())
        break;
    }
    throwEnvelopeError(parsed.status, wire.substr(parsed.bodyStart));
  }

  std::string::size_type cursor = parsed.bodyStart;
  for (;;) {
    std::string::size_type lineEnd = wire.find("\r\n", cursor);
    while (lineEnd == std::string::npos) {
      if (!recvMore())
        throw std::runtime_error("argus-tts chunked body truncated");
      lineEnd = wire.find("\r\n", cursor);
    }
    const std::string sizeLine = wire.substr(cursor, lineEnd - cursor);
    const auto semicolon = sizeLine.find(';');
    const std::string sizeHex =
        trim(semicolon == std::string::npos
                 ? sizeLine
                 : sizeLine.substr(0, semicolon));
    if (sizeHex.empty())
      throw std::runtime_error("argus-tts malformed chunk size");
    const size_t size =
        static_cast<size_t>(std::stoul(sizeHex, nullptr, 16));
    if (size == 0)
      return;
    const auto dataStart = lineEnd + 2;
    while (wire.size() < dataStart + size + 2) {
      if (!recvMore())
        throw std::runtime_error("argus-tts chunked body truncated");
    }
    onChunk(wire.data() + dataStart, size);
    cursor = dataStart + size + 2;
  }
}

float TtsHttpClient::defaultSpeed() const
{
  const RawResponse response = exchange(
      {.method = "GET", .path = "/tts/v1/config", .body = "",
       .closeConnection = true});
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("defaultSpeed"))
    throw std::runtime_error("argus-tts config response unreadable");
  return json["info"]["defaultSpeed"].asFloat();
}

int TtsHttpClient::sampleRate() const
{
  const RawResponse response = exchange(
      {.method = "GET", .path = "/tts/v1/config", .body = "",
       .closeConnection = true});
  Json::Value json;
  Json::Reader reader;
  if (!reader.parse(response.body, json) || !json["info"].isObject() ||
      !json["info"].isMember("sampleRate"))
    throw std::runtime_error("argus-tts config response unreadable");
  return json["info"]["sampleRate"].asInt();
}

std::vector<float> TtsHttpClient::synthesize(const TtsRequest& req) const
{
  const RawResponse response =
      exchange({.method = "POST",
                .path = "/tts/v1/synthesize",
                .body = jsonBody(req),
                .closeConnection = true});
  return pcmFromBytes(response.body);
}

void TtsHttpClient::synthesizeStream(const TtsRequest& req,
                                     TtsChunkCallback onChunk) const
{
  std::vector<float> pending;
  stream({.method = "POST",
          .path = "/tts/v1/synthesize-stream",
          .body = jsonBody(req),
          .closeConnection = false},
         [&pending, &onChunk](const char* data, size_t size) {
           if (size % sizeof(float) != 0)
             throw std::runtime_error(
                 "argus-tts stream chunk is not float32-aligned");
           pending.resize(size / sizeof(float));
           std::memcpy(pending.data(), data, size);
           onChunk(pending);
         });
}

float TtsClient::defaultSpeed() const
{
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).defaultSpeed();
}

int TtsClient::sampleRate() const
{
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).sampleRate();
}

std::vector<float> TtsClient::synthesize(const TtsRequest& req) const
{
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  return TtsHttpClient(config.url, config.timeoutMs).synthesize(req);
}

void TtsClient::synthesizeStream(const TtsRequest& req,
                                 TtsChunkCallback onChunk) const
{
  const auto config = TtsRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("tts.remote_url is not configured");
  TtsHttpClient(config.url, config.timeoutMs)
      .synthesizeStream(req, std::move(onChunk));
}

bool TtsClient::remote() const
{
  return TtsRemoteConfig::resolve().enabled();
}
