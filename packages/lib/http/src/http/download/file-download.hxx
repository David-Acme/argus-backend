#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <http/download/chunk-transport.hxx>
#include <memory>
#include <runtime/cancellation-token.hxx>
#include <string>
#include <string_view>

namespace file_download
{

enum class DownloadStatus : std::uint8_t
{
  Done,
  Cancelled,
  Failed,
};

enum class DownloadFailure : std::uint8_t
{
  None,
  InvalidRequest,
  OnEventLoop,
  Busy,
  Filesystem,
  Network,
  Tls,
  HttpStatus,
  RedirectLoop,
  RangeUnsupported,
  RangeMismatch,
  SourceChanged,
  SizeMismatch,
  Oversize,
  HashMismatch,
};

[[nodiscard]] std::string_view downloadFailureToString(DownloadFailure failure);

struct DownloadProgress
{
  std::uint64_t bytesPresent = 0;
  std::uint64_t bytesTotal = 0;
};

struct DownloadPolicy
{
  std::uint64_t chunkBytes = std::uint64_t{8} << 20U;
  std::uint64_t singleRequestLimit = std::uint64_t{16} << 20U;
  std::chrono::milliseconds requestTimeout{60'000};
  int maxRedirects = 8;
  int maxReResolves = 3;
  int maxAttempts = 6;
  std::chrono::milliseconds backoffInitial{500};
  std::chrono::milliseconds backoffMax{15'000};
};

struct DownloadRequest
{
  std::string url;
  std::filesystem::path target;
  std::uint64_t expectedSize = 0;
  std::string expectedSha256;
  CancellationToken cancellation;
  std::function<void(const DownloadProgress&)> onProgress;
  DownloadPolicy policy;
  std::shared_ptr<ChunkTransport> transport;
};

struct DownloadResult
{
  DownloadStatus status = DownloadStatus::Failed;
  DownloadFailure failure = DownloadFailure::None;
  std::uint64_t bytesPresent = 0;
  std::uint64_t bytesFetched = 0;
  int httpStatus = 0;
  std::string detail;
};

[[nodiscard]] DownloadResult downloadFile(const DownloadRequest& request);

[[nodiscard]] std::uint64_t presentBytes(const std::filesystem::path& target);

[[nodiscard]] std::filesystem::path partPath(const std::filesystem::path& target);

[[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& target);

}
