#include "upstream-http.hxx"

#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace upstream_http
{

Upstream open(const std::string& host, int port, const std::string& path,
              int timeoutSec)
{
  Upstream up;
  up.fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (up.fd < 0)
    return up;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  timeval tv{};
  tv.tv_sec = timeoutSec;
  ::setsockopt(up.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(up.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const int one = 1;
  ::setsockopt(up.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  if (::connect(up.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nConnection: close\r\nUser-Agent: argus\r\n\r\n";
  if (::send(up.fd, req.data(), req.size(), 0) < 0) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  std::string buf;
  char tmp[4096];
  while (buf.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(up.fd, tmp, sizeof(tmp), 0);
    if (n <= 0) {
      ::close(up.fd);
      up.fd = -1;
      return up;
    }
    buf.append(tmp, static_cast<size_t>(n));
    if (buf.size() > 16384)
      break;
  }

  const auto sep = buf.find("\r\n\r\n");
  if (sep == std::string::npos) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }
  up.headers = buf.substr(0, sep);
  up.leftover = buf.substr(sep + 4);
  up.ok = up.headers.find(" 200") != std::string::npos;
  if (!up.ok) {
    ::close(up.fd);
    up.fd = -1;
  }
  return up;
}

std::pair<std::string, int> splitHostPort(const std::string& addr)
{
  const auto colon = addr.find(':');
  if (colon == std::string::npos)
    return {addr, 80};
  return {addr.substr(0, colon), std::atoi(addr.c_str() + colon + 1)};
}

namespace
{

uint64_t boxSize(const uint8_t* data)
{
  const uint64_t size = (static_cast<uint64_t>(data[0]) << 24) |
                        (static_cast<uint64_t>(data[1]) << 16) |
                        (static_cast<uint64_t>(data[2]) << 8) |
                        static_cast<uint64_t>(data[3]);
  if (size == 1)
    return (static_cast<uint64_t>(data[8]) << 56) |
           (static_cast<uint64_t>(data[9]) << 48) |
           (static_cast<uint64_t>(data[10]) << 40) |
           (static_cast<uint64_t>(data[11]) << 32) |
           (static_cast<uint64_t>(data[12]) << 24) |
           (static_cast<uint64_t>(data[13]) << 16) |
           (static_cast<uint64_t>(data[14]) << 8) |
           static_cast<uint64_t>(data[15]);
  return size;
}

size_t parseChunkSize(const std::string& line, bool& ok)
{
  ok = false;
  size_t end = 0;
  while (end < line.size() &&
         (std::isxdigit(static_cast<unsigned char>(line[end])))) {
    ++end;
  }
  if (end == 0)
    return 0;
  const std::string hex = line.substr(0, end);
  errno = 0;
  char* endptr = nullptr;
  const unsigned long value = std::strtoul(hex.c_str(), &endptr, 16);
  if (errno != 0 || endptr == hex.c_str())
    return 0;
  ok = true;
  return static_cast<size_t>(value);
}

} // namespace

void Fmp4Reader::feed(const char* data, size_t len)
{
  if (len == 0)
    return;

  if (!chunked_) {
    size_t i = 0;
    while (i < len && data[i] != '\n')
      ++i;
    if (i == len) {
      lineBuf_.append(data, len);
      return;
    }
    lineBuf_.append(data, i + 1);
    if (lineBuf_.size() < 2 || lineBuf_[lineBuf_.size() - 2] != '\r') {
      lineBuf_.clear();
      pending_.append(data, len);
      consume();
      return;
    }
    bool ok = false;
    const size_t size = parseChunkSize(lineBuf_, ok);
    lineBuf_.clear();
    if (!ok) {
      pending_.append(data, len);
      consume();
      return;
    }
    if (size == 0)
      return;
    chunked_ = true;
    chunkRemaining_ = size;
    processChunked(data + i + 1, len - (i + 1));
    return;
  }

  processChunked(data, len);
}

void Fmp4Reader::processChunked(const char* data, size_t len)
{
  size_t pos = 0;
  while (pos < len) {
    if (chunkRemaining_ > 0) {
      const size_t take = std::min(chunkRemaining_, len - pos);
      pending_.append(data + pos, take);
      consume();
      chunkRemaining_ -= take;
      pos += take;
      if (chunkRemaining_ == 0)
        afterChunk_ = true;
      continue;
    }

    if (afterChunk_) {
      const size_t need = 2;
      if (len - pos < need)
        return;
      pos += need;
      afterChunk_ = false;
      lineBuf_.clear();
      continue;
    }

    const char* nl =
        static_cast<const char*>(std::memchr(data + pos, '\n', len - pos));
    if (nl == nullptr) {
      lineBuf_.append(data + pos, len - pos);
      return;
    }
    const size_t consumed = static_cast<size_t>(nl - (data + pos)) + 1;
    lineBuf_.append(data + pos, consumed);
    pos += consumed;

    if (!lineBuf_.empty() && lineBuf_.back() == '\n' &&
        (lineBuf_.size() < 2 || lineBuf_[lineBuf_.size() - 2] != '\r')) {
      lineBuf_.clear();
      continue;
    }
    bool ok = false;
    const size_t size = parseChunkSize(lineBuf_, ok);
    lineBuf_.clear();
    if (!ok)
      return;
    if (size == 0)
      return;
    chunkRemaining_ = size;
    afterChunk_ = false;
  }
}

void Fmp4Reader::reset()
{
  pending_.clear();
  init_.clear();
  initDone_ = false;
  chunked_ = false;
  lineBuf_.clear();
  chunkRemaining_ = 0;
  afterChunk_ = false;
}

void Fmp4Reader::consume()
{
  while (pending_.size() >= 8) {
    const auto* raw = reinterpret_cast<const uint8_t*>(pending_.data());
    const uint64_t size = boxSize(raw);
    const size_t headerLen = size == 1 ? 16 : 8;
    if (size == 0 || size < headerLen) {
      pending_.clear();
      return;
    }
    if (pending_.size() < size)
      return;

    const char type[5] = {static_cast<char>(raw[4]), static_cast<char>(raw[5]),
                          static_cast<char>(raw[6]), static_cast<char>(raw[7]),
                          '\0'};

    std::string box = pending_.substr(0, static_cast<size_t>(size));
    pending_.erase(0, static_cast<size_t>(size));

    if (!initDone_) {
      init_ += box;
      if (std::strcmp(type, "moov") == 0) {
        initDone_ = true;
        if (onInit)
          onInit(std::move(init_));
      }
      continue;
    }

    const bool keyframe = std::strcmp(type, "moof") == 0;
    if (onBox)
      onBox(std::move(box), keyframe);
  }
}

} // namespace upstream_http
