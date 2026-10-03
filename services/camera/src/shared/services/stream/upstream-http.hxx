#pragma once

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
};

Upstream open(const OpenInput& input);

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

  void feed(const char* data, size_t len);
  void reset();
  bool initDone() const { return initDone_; }

private:
  void consume();
  void processChunked(const char* data, size_t len);
  void emit(std::string box, const std::string& type);

  std::string pending_;
  std::string init_;
  std::string fragment_;
  bool initDone_{false};
  bool hasMoof_{false};
  FragmentKind fragmentKind_{FragmentKind::Other};
  uint32_t videoTrack_{0};

  bool chunked_{false};
  std::string lineBuf_;
  size_t chunkRemaining_{0};
  bool inChunkData_{false};
};

}
