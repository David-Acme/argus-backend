#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace upstream_http
{

struct Upstream
{
  int fd{-1};
  std::string headers;
  std::string leftover;
  bool ok{false};
};

Upstream open(const std::string& host, int port, const std::string& path,
              int timeoutSec);

std::pair<std::string, int> splitHostPort(const std::string& addr);

bool isChunked(const std::string& headers);

struct Fmp4ReaderInput
{
  bool chunked;
};

class Fmp4Reader
{
public:
  std::function<void(std::string init)> onInit;
  std::function<void(std::string fragment, bool keyframe)> onFragment;

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
  bool fragmentKeyframe_{false};

  bool chunked_{false};
  std::string lineBuf_;
  size_t chunkRemaining_{0};
  bool inChunkData_{false};
};

} // namespace upstream_http
