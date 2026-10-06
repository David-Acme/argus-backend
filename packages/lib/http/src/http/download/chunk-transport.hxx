#pragma once

#include <chrono>
#include <cstdint>
#include <runtime/cancellation-token.hxx>
#include <string>

namespace file_download
{

enum class TransportError : std::uint8_t
{
  None,
  Network,
  Timeout,
  Tls,
  InvalidUrl,
  Cancelled,
};

struct ChunkRequest
{
  std::string url;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::chrono::milliseconds timeout{0};
  CancellationToken cancellation;
};

struct ChunkResponse
{
  TransportError error = TransportError::None;
  int status = 0;
  std::string contentRange;
  std::string etag;
  std::string location;
  std::string body;
  std::string detail;
};

class ChunkTransport
{
public:
  ChunkTransport() = default;
  ChunkTransport(const ChunkTransport&) = delete;
  ChunkTransport& operator=(const ChunkTransport&) = delete;
  ChunkTransport(ChunkTransport&&) = delete;
  ChunkTransport& operator=(ChunkTransport&&) = delete;
  virtual ~ChunkTransport() = default;

  [[nodiscard]] virtual ChunkResponse fetch(const ChunkRequest& request) = 0;
};

}
