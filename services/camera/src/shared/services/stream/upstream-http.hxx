#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace upstream_http
{

struct Upstream
{
  int fd{-1};
  std::string headers;
  std::string leftover;
  bool ok{false};
};

struct OpenInput
{
  std::string host;
  int port{0};
  std::string path;
  int timeoutSec{0};
  const std::atomic<bool>* cancel{nullptr};
};

Upstream open(const OpenInput& input);

bool isOkStatusLine(std::string_view headers);

std::pair<std::string, int> splitHostPort(const std::string& addr);

bool isChunked(const std::string& headers);

struct Fmp4ReaderInput
{
  bool chunked;
};

enum class FragmentKind : uint8_t
{
  VideoKey,
  VideoDelta,
  Other,
};

struct Fmp4Fragment
{
  std::string bytes;
  FragmentKind kind{FragmentKind::Other};
};

uint32_t videoTrackOf(std::string_view moov);

struct FragmentKindInput
{
  std::string_view moof;
  uint32_t videoTrack{0};
};

FragmentKind fragmentKindOf(const FragmentKindInput& input);

class Fmp4Reader
{
public:
  std::function<void(std::string init)> onInit;
  std::function<void(Fmp4Fragment fragment)> onFragment;

  explicit Fmp4Reader(Fmp4ReaderInput input);

  static constexpr size_t kMaxInitBytes = size_t{4} * 1024 * 1024;

  void feed(const char* data, size_t len);
  void reset();
  bool initDone() const { return initDone_; }
  bool corrupt() const { return corrupt_; }

private:
  struct BoxView
  {
    size_t offset{0};
    size_t size{0};
    std::string_view type;
  };

  void consume();
  void processChunked(const char* data, size_t len);
  void emit(const BoxView& box);
  void fail();

  std::string pending_;
  size_t offset_{0};
  std::string init_;
  std::string fragment_;
  bool initDone_{false};
  bool corrupt_{false};
  bool hasMoof_{false};
  FragmentKind fragmentKind_{FragmentKind::Other};
  uint32_t videoTrack_{0};

  bool chunked_{false};
  std::string lineBuf_;
  size_t chunkRemaining_{0};
  bool inChunkData_{false};
};

}
