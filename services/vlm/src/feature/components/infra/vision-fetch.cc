#include "vision-fetch.hxx"

#include <runtime/cancellation-token.hxx>

#include <stop_token>
#include <system_error>
#include <utility>

namespace
{
using file_download::DownloadFailure;

bool diskShort(const FetchFailureInput& input)
{
  std::error_code error;
  const auto space = std::filesystem::space(input.target.parent_path(), error);
  return !error && std::cmp_less(space.available, input.remainingBytes);
}
}

std::string fetchFailureReason(const FetchFailureInput& input)
{
  switch (input.failure) {
  case DownloadFailure::None: return {};
  case DownloadFailure::Network: return "network";
  case DownloadFailure::Filesystem: return diskShort(input) ? "disk_full" : "filesystem";
  case DownloadFailure::HashMismatch:
  case DownloadFailure::SizeMismatch:
  case DownloadFailure::Oversize: return "checksum_mismatch";
  case DownloadFailure::Tls:
  case DownloadFailure::HttpStatus:
  case DownloadFailure::RedirectLoop:
  case DownloadFailure::RangeUnsupported:
  case DownloadFailure::RangeMismatch:
  case DownloadFailure::SourceChanged: return "source_unavailable";
  case DownloadFailure::InvalidRequest:
  case DownloadFailure::OnEventLoop:
  case DownloadFailure::Busy: return std::string(file_download::downloadFailureToString(input.failure));
  }
  return "network";
}

ComponentFetch visionFetch(VisionFetchInput input)
{
  return [input = std::move(input)](const ComponentFetchInput& fetch) {
    const CancellationToken cancellation;
    const std::stop_callback onStop(fetch.stop, [&cancellation] { cancellation.cancel(); });
    const auto result = file_download::downloadFile({.url = fetch.file.url,
                                                     .target = fetch.target,
                                                     .expectedSize = static_cast<std::uint64_t>(fetch.file.sizeBytes),
                                                     .expectedSha256 = fetch.file.sha256,
                                                     .cancellation = cancellation,
                                                     .onProgress = {},
                                                     .policy = input.policy,
                                                     .transport = input.transport});
    if (result.status != file_download::DownloadStatus::Failed)
      return std::string{};
    return fetchFailureReason({.failure = result.failure,
                               .target = fetch.target,
                               .remainingBytes = fetch.file.sizeBytes -
                                                 static_cast<std::int64_t>(result.bytesPresent)});
  };
}
