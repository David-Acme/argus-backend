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

class Fmp4Reader
{
public:
  std::function<void(std::string box)> onInit;
  std::function<void(std::string box, bool keyframe)> onBox;

  void feed(const char* data, size_t len);
  void reset();
  bool initDone() const { return initDone_; }

private:
  void consume();
  void processChunked(const char* data, size_t len);

  std::string pending_;
  std::string init_;
  bool initDone_{false};

  bool chunked_{false};
  std::string lineBuf_;
  size_t chunkRemaining_{0};
  bool afterChunk_{false};
};

} // namespace upstream_http
