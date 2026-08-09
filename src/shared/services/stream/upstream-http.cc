#include "upstream-http.hxx"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
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

bool isChunked(const std::string& headers)
{
  std::string lower = headers;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return lower.find("transfer-encoding: chunked") != std::string::npos;
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

uint32_t readBe32(const uint8_t* data)
{
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
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

bool syncFromSampleFlags(uint32_t flags)
{
  return (flags & 0x00010000U) == 0;
}

bool moofIsKeyframe(const std::string& moof)
{
  bool found = false;
  bool sync = true;
  const auto* raw = reinterpret_cast<const uint8_t*>(moof.data());
  const size_t total = moof.size();

  std::function<void(size_t, size_t)> walk = [&](size_t begin, size_t end) {
    size_t offset = begin;
    while (offset + 8 <= end) {
      const uint64_t size = (static_cast<uint64_t>(raw[offset]) << 24) |
                            (static_cast<uint64_t>(raw[offset + 1]) << 16) |
                            (static_cast<uint64_t>(raw[offset + 2]) << 8) |
                            static_cast<uint64_t>(raw[offset + 3]);
      if (size < 8 || offset + size > end)
        return;
      const std::string type(moof, offset + 4, 4);
      const size_t body = offset + 8;
      if (type == "moof" || type == "traf")
        walk(body, offset + size);
      else if (type == "tfhd" && body + 8 <= end) {
        const uint32_t tf = readBe32(raw + body) & 0x00FFFFFFU;
        size_t cursor = body + 8;
        if (tf & 0x000001U)
          cursor += 8;
        if (tf & 0x000002U)
          cursor += 4;
        if (tf & 0x000008U)
          cursor += 4;
        if (tf & 0x000010U)
          cursor += 4;
        if ((tf & 0x000020U) && cursor + 4 <= end) {
          sync = syncFromSampleFlags(readBe32(raw + cursor));
          found = true;
        }
      }
      else if (type == "trun" && body + 8 <= end) {
        const uint32_t tr = readBe32(raw + body) & 0x00FFFFFFU;
        size_t cursor = body + 8;
        if (tr & 0x000001U)
          cursor += 4;
        if ((tr & 0x000004U) && cursor + 4 <= end) {
          sync = syncFromSampleFlags(readBe32(raw + cursor));
          found = true;
        }
      }
      offset += size;
    }
  };

  walk(0, total);
  return found ? sync : true;
}

} // namespace

Fmp4Reader::Fmp4Reader(Fmp4ReaderInput input) : chunked_(input.chunked) {}

void Fmp4Reader::feed(const char* data, size_t len)
{
  if (len == 0)
    return;
  if (!chunked_) {
    pending_.append(data, len);
    consume();
    return;
  }
  processChunked(data, len);
}

void Fmp4Reader::processChunked(const char* data, size_t len)
{
  size_t pos = 0;
  while (pos < len) {
    if (inChunkData_) {
      const size_t take = std::min(chunkRemaining_, len - pos);
      pending_.append(data + pos, take);
      chunkRemaining_ -= take;
      pos += take;
      if (chunkRemaining_ == 0) {
        inChunkData_ = false;
        lineBuf_.clear();
      }
      consume();
      continue;
    }

    lineBuf_ += data[pos];
    ++pos;
    if (lineBuf_.size() > 64) {
      lineBuf_.clear();
      continue;
    }
    if (lineBuf_.size() < 2 || lineBuf_[lineBuf_.size() - 1] != '\n' ||
        lineBuf_[lineBuf_.size() - 2] != '\r')
      continue;

    const std::string line = lineBuf_.substr(0, lineBuf_.size() - 2);
    lineBuf_.clear();
    if (line.empty())
      continue;
    bool ok = false;
    const size_t size = parseChunkSize(line, ok);
    if (!ok || size == 0)
      continue;
    chunkRemaining_ = size;
    inChunkData_ = true;
  }
}

void Fmp4Reader::emit(std::string box, const std::string& type)
{
  if (!initDone_) {
    init_ += box;
    if (type == "moov") {
      initDone_ = true;
      if (onInit)
        onInit(std::move(init_));
      init_.clear();
    }
    return;
  }

  if (type == "moof") {
    fragment_ = std::move(box);
    fragmentKeyframe_ = moofIsKeyframe(fragment_);
    hasMoof_ = true;
    return;
  }

  if (type == "mdat" && hasMoof_) {
    fragment_ += box;
    hasMoof_ = false;
    if (onFragment)
      onFragment(std::move(fragment_), fragmentKeyframe_);
    fragment_.clear();
  }
}

void Fmp4Reader::reset()
{
  pending_.clear();
  init_.clear();
  fragment_.clear();
  initDone_ = false;
  hasMoof_ = false;
  chunked_ = false;
  lineBuf_.clear();
  chunkRemaining_ = 0;
  inChunkData_ = false;
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

    const std::string type(pending_, 4, 4);
    std::string box = pending_.substr(0, static_cast<size_t>(size));
    pending_.erase(0, static_cast<size_t>(size));

    emit(std::move(box), type);
  }
}

} // namespace upstream_http
