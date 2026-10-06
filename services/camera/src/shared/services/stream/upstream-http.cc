#include "upstream-http.hxx"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace upstream_http
{

namespace
{
constexpr int kCancelSliceMs = 100;

bool cancelled(const OpenInput& input)
{
  return input.cancel != nullptr && input.cancel->load(std::memory_order_acquire);
}

bool waitFor(const OpenInput& input, int fd, short events)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(input.timeoutSec);
  pollfd descriptor{.fd = fd, .events = events, .revents = 0};
  while (!cancelled(input) && std::chrono::steady_clock::now() < deadline) {
    const int ready = ::poll(&descriptor, 1, kCancelSliceMs);
    if (ready > 0)
      return true;
    if (ready < 0 && errno != EINTR)
      return false;
  }
  return false;
}

Upstream closed(Upstream up)
{
  if (up.fd >= 0)
    ::close(up.fd);
  up.fd = -1;
  up.ok = false;
  return up;
}
}

bool isOkStatusLine(std::string_view headers)
{
  const std::string_view line = headers.substr(0, headers.find("\r\n"));
  if (!line.starts_with("HTTP/1.") || line.size() < 12)
    return false;
  return line.substr(8, 4) == " 200" && (line.size() == 12 || line[12] == ' ');
}

Upstream open(const OpenInput& input)
{
  Upstream up;
  up.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (up.fd < 0)
    return up;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(input.port));
  if (::inet_pton(AF_INET, input.host.c_str(), &addr.sin_addr) != 1)
    return closed(std::move(up));

  const int one = 1;
  ::setsockopt(up.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  if (::connect(up.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS || !waitFor(input, up.fd, POLLOUT))
      return closed(std::move(up));
    int status = 0;
    socklen_t length = sizeof(status);
    if (::getsockopt(up.fd, SOL_SOCKET, SO_ERROR, &status, &length) != 0 || status != 0)
      return closed(std::move(up));
  }

  const std::string req = "GET " + input.path + " HTTP/1.1\r\nHost: " + input.host +
                          "\r\nConnection: close\r\nUser-Agent: argus\r\n\r\n";
  std::string_view pending = req;
  while (!pending.empty()) {
    const ssize_t sent = ::send(up.fd, pending.data(), pending.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      pending.remove_prefix(static_cast<size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(input, up.fd, POLLOUT))
      continue;
    return closed(std::move(up));
  }

  std::string buf;
  std::array<char, 4096> chunk{};
  while (buf.find("\r\n\r\n") == std::string::npos) {
    if (buf.size() > 16384)
      return closed(std::move(up));
    const auto n = ::recv(up.fd, chunk.data(), chunk.size(), 0);
    if (n > 0) {
      buf.append(chunk.data(), static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(input, up.fd, POLLIN))
      continue;
    return closed(std::move(up));
  }

  const auto sep = buf.find("\r\n\r\n");
  up.headers = buf.substr(0, sep);
  up.leftover = buf.substr(sep + 4);
  up.ok = isOkStatusLine(up.headers);
  if (!up.ok)
    return closed(std::move(up));
  const int flags = ::fcntl(up.fd, F_GETFL, 0);
  ::fcntl(up.fd, F_SETFL,
          static_cast<int>(static_cast<unsigned>(flags) & ~static_cast<unsigned>(O_NONBLOCK)));
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

constexpr uint32_t kNonSyncSample = 0x00010000U;

uint32_t be32(std::string_view data, size_t offset)
{
  if (offset + 4 > data.size())
    return 0;
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | static_cast<uint8_t>(data[offset + i]);
  return value;
}

struct Box
{
  std::string_view type;
  std::string_view body;
};

std::vector<Box> childrenOf(std::string_view data)
{
  std::vector<Box> boxes;
  size_t offset = 0;
  while (offset + 8 <= data.size()) {
    const uint32_t size = be32(data, offset);
    if (size < 8 || offset + size > data.size())
      break;
    boxes.push_back({.type = data.substr(offset + 4, 4),
                     .body = data.substr(offset + 8, size - 8)});
    offset += size;
  }
  return boxes;
}

uint32_t trackIdOf(std::string_view tkhd)
{
  const bool wide = !tkhd.empty() && static_cast<uint8_t>(tkhd[0]) == 1;
  return be32(tkhd, wide ? 20 : 12);
}

bool isVideoMedia(std::string_view mdia)
{
  return std::ranges::any_of(childrenOf(mdia), [](const Box& box) {
    return box.type == "hdlr" && box.body.size() >= 12 &&
           box.body.substr(8, 4) == "vide";
  });
}

struct TrackFragment
{
  uint32_t track{0};
  bool sync{true};
};

TrackFragment trackFragmentOf(std::string_view traf)
{
  TrackFragment fragment;
  std::optional<uint32_t> flags;
  for (const Box& part : childrenOf(traf)) {
    if (part.type == "tfhd") {
      const uint32_t tf = be32(part.body, 0) & 0x00FFFFFFU;
      fragment.track = be32(part.body, 4);
      size_t cursor = 8;
      if (tf & 0x000001U)
        cursor += 8;
      if (tf & 0x000002U)
        cursor += 4;
      if (tf & 0x000008U)
        cursor += 4;
      if (tf & 0x000010U)
        cursor += 4;
      if ((tf & 0x000020U) && cursor + 4 <= part.body.size())
        flags = be32(part.body, cursor);
    }
    else if (part.type == "trun") {
      const uint32_t tr = be32(part.body, 0) & 0x00FFFFFFU;
      const uint32_t samples = be32(part.body, 4);
      size_t cursor = 8;
      if (tr & 0x000001U)
        cursor += 4;
      if ((tr & 0x000004U) && cursor + 4 <= part.body.size()) {
        flags = be32(part.body, cursor);
      }
      else if (samples > 0 && (tr & 0x000400U)) {
        size_t sample = cursor;
        if (tr & 0x000100U)
          sample += 4;
        if (tr & 0x000200U)
          sample += 4;
        if (sample + 4 <= part.body.size())
          flags = be32(part.body, sample);
      }
    }
  }
  fragment.sync = !flags || (*flags & kNonSyncSample) == 0;
  return fragment;
}

}

uint32_t videoTrackOf(std::string_view moov)
{
  for (const Box& top : childrenOf(moov)) {
    if (top.type != "moov")
      continue;
    for (const Box& trak : childrenOf(top.body)) {
      if (trak.type != "trak")
        continue;
      uint32_t track = 0;
      bool video = false;
      for (const Box& part : childrenOf(trak.body)) {
        if (part.type == "tkhd")
          track = trackIdOf(part.body);
        else if (part.type == "mdia")
          video = isVideoMedia(part.body);
      }
      if (video && track != 0)
        return track;
    }
  }
  return 0;
}

FragmentKind fragmentKindOf(const FragmentKindInput& input)
{
  for (const Box& top : childrenOf(input.moof)) {
    if (top.type != "moof")
      continue;
    for (const Box& traf : childrenOf(top.body)) {
      if (traf.type != "traf")
        continue;
      const TrackFragment fragment = trackFragmentOf(traf.body);
      if (input.videoTrack == 0 || fragment.track == input.videoTrack)
        return fragment.sync ? FragmentKind::VideoKey : FragmentKind::VideoDelta;
    }
  }
  return FragmentKind::Other;
}

Fmp4Reader::Fmp4Reader(Fmp4ReaderInput input) : chunked_(input.chunked) {}

void Fmp4Reader::feed(const char* data, size_t len)
{
  if (len == 0 || corrupt_)
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

void Fmp4Reader::fail()
{
  reset();
  corrupt_ = true;
}

void Fmp4Reader::emit(const BoxView& box)
{
  const std::string_view bytes(pending_.data() + box.offset, box.size);
  if (!initDone_) {
    if (init_.size() + bytes.size() > kMaxInitBytes) {
      fail();
      return;
    }
    init_.append(bytes);
    if (box.type == "moov") {
      initDone_ = true;
      videoTrack_ = videoTrackOf(init_);
      if (onInit)
        onInit(std::move(init_));
      init_.clear();
    }
    return;
  }

  if (box.type == "moof") {
    fragment_.assign(bytes);
    fragmentKind_ = fragmentKindOf({.moof = fragment_, .videoTrack = videoTrack_});
    hasMoof_ = true;
    return;
  }

  if (box.type == "mdat" && hasMoof_) {
    fragment_.append(bytes);
    hasMoof_ = false;
    if (onFragment)
      onFragment({.bytes = std::move(fragment_), .kind = fragmentKind_});
    fragment_ = std::string();
  }
}

void Fmp4Reader::reset()
{
  pending_.clear();
  offset_ = 0;
  corrupt_ = false;
  init_.clear();
  fragment_.clear();
  initDone_ = false;
  hasMoof_ = false;
  fragmentKind_ = FragmentKind::Other;
  videoTrack_ = 0;
  chunked_ = false;
  lineBuf_.clear();
  chunkRemaining_ = 0;
  inChunkData_ = false;
}

void Fmp4Reader::consume()
{
  constexpr uint64_t kMaxBoxBytes = 64ULL * 1024 * 1024;
  while (!corrupt_ && pending_.size() - offset_ >= 8) {
    const std::string_view window(pending_.data() + offset_, pending_.size() - offset_);
    const uint32_t compact = be32(window, 0);
    uint64_t size = compact;
    if (compact == 1) {
      if (window.size() < 16)
        break;
      size = (static_cast<uint64_t>(be32(window, 8)) << 32U) | static_cast<uint64_t>(be32(window, 12));
    }
    const uint64_t headerLen = compact == 1 ? 16 : 8;
    if (size < headerLen || size > kMaxBoxBytes) {
      fail();
      return;
    }
    if (window.size() < size)
      break;
    emit({.offset = offset_, .size = static_cast<size_t>(size), .type = window.substr(4, 4)});
    if (corrupt_)
      return;
    offset_ += static_cast<size_t>(size);
  }
  if (offset_ > 0 && (offset_ == pending_.size() || offset_ >= pending_.size() / 2)) {
    pending_.erase(0, offset_);
    offset_ = 0;
  }
}

}
