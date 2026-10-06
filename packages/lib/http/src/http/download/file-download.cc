#include "file-download.hxx"

#include "details/download-sidecar.hxx"
#include "details/download-url.hxx"
#include "details/part-file.hxx"
#include "details/sha256-stream.hxx"
#include "drogon-chunk-transport.hxx"

#include <algorithm>
#include <fstream>
#include <optional>
#include <thread>
#include <trantor/net/EventLoop.h>
#include <utility>
#include <vector>

namespace file_download
{
namespace
{

using namespace std::chrono_literals;

constexpr std::size_t kHashBlock = std::size_t{1} << 20U;
constexpr std::size_t kShaHexLength = 64;
constexpr auto kBackoffPoll = 20ms;
constexpr int kPartialContent = 206;
constexpr int kOk = 200;

enum class FeedOutcome : std::uint8_t
{
  Fed,
  Cancelled,
  Failed,
};

struct FeedInput
{
  details::Sha256Stream& hasher;
  const std::filesystem::path& path;
  std::uint64_t length = 0;
  const CancellationToken& cancellation;
};

FeedOutcome feedFile(const FeedInput& input)
{
  std::ifstream stream(input.path, std::ios::binary);
  if (!stream)
    return FeedOutcome::Failed;
  std::vector<char> block(kHashBlock);
  std::uint64_t remaining = input.length;
  while (remaining > 0)
  {
    if (input.cancellation.cancelled())
      return FeedOutcome::Cancelled;
    const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, block.size()));
    stream.read(block.data(), want);
    if (stream.gcount() != want)
      return FeedOutcome::Failed;
    input.hasher.update(std::span(block).first(static_cast<std::size_t>(want)));
    remaining -= static_cast<std::uint64_t>(want);
  }
  return FeedOutcome::Fed;
}

bool isLowerSha256(std::string_view text)
{
  return text.size() == kShaHexLength &&
         std::ranges::all_of(text, [](char c)
                             { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isRedirect(int status)
{
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool isTransientStatus(int status) { return status == 408 || status == 429 || status >= 500; }

struct Outcome
{
  DownloadStatus status = DownloadStatus::Failed;
  DownloadFailure failure = DownloadFailure::None;
  std::string detail;
};

enum class StepKind : std::uint8_t
{
  Advanced,
  Again,
  Retry,
  Stop,
};

struct Step
{
  StepKind kind = StepKind::Stop;
  Outcome outcome;
};

Step advanced() { return {.kind = StepKind::Advanced, .outcome = {}}; }

Step again() { return {.kind = StepKind::Again, .outcome = {}}; }

Step retry(DownloadFailure failure, std::string detail)
{
  return {.kind = StepKind::Retry,
          .outcome = {.status = DownloadStatus::Failed, .failure = failure, .detail = std::move(detail)}};
}

Step stop(DownloadFailure failure, std::string detail)
{
  return {.kind = StepKind::Stop,
          .outcome = {.status = DownloadStatus::Failed, .failure = failure, .detail = std::move(detail)}};
}

Step cancelledStep()
{
  return {.kind = StepKind::Stop,
          .outcome = {.status = DownloadStatus::Cancelled, .failure = DownloadFailure::None, .detail = {}}};
}

struct RangeTarget
{
  std::string url;
  std::uint64_t last = 0;
};

class Transfer
{
public:
  Transfer(const DownloadRequest& request, ChunkTransport& transport)
      : request_(request), transport_(transport), part_(partPath(request.target)),
        sidecar_(sidecarPath(request.target))
  {
  }

  DownloadResult run()
  {
    if (auto invalid = validate())
      return finish(*std::move(invalid));
    if (auto existing = acceptExistingTarget())
      return finish(*std::move(existing));
    if (auto opened = openPart())
      return finish(*std::move(opened));
    if (auto restored = restore())
      return finish(*std::move(restored));
    report();
    if (auto fetched = fetchAll())
    {
      if (present_ == 0 && file_.isOpen())
        discard();
      return finish(*std::move(fetched));
    }
    return finish(commit());
  }

private:
  [[nodiscard]] std::optional<Outcome> validate() const
  {
    const auto& policy = request_.policy;
    const bool valid = details::splitUrl(request_.url).has_value() && !request_.target.empty() &&
                       request_.target.has_filename() && request_.expectedSize > 0 &&
                       isLowerSha256(request_.expectedSha256) && policy.chunkBytes > 0 &&
                       policy.maxAttempts > 0 && policy.maxRedirects >= 0 &&
                       policy.maxReResolves >= 0 && policy.requestTimeout > 0ms;
    if (valid)
      return std::nullopt;
    return Outcome{.status = DownloadStatus::Failed,
                   .failure = DownloadFailure::InvalidRequest,
                   .detail = "url, target, size, lowercase sha256 and policy must be valid"};
  }

  std::optional<Outcome> acceptExistingTarget()
  {
    std::error_code error;
    const auto size = std::filesystem::file_size(request_.target, error);
    if (error || size != request_.expectedSize)
      return std::nullopt;
    details::Sha256Stream hasher;
    const auto fed = feedFile({.hasher = hasher,
                               .path = request_.target,
                               .length = size,
                               .cancellation = request_.cancellation});
    if (fed == FeedOutcome::Cancelled)
      return Outcome{.status = DownloadStatus::Cancelled, .failure = DownloadFailure::None, .detail = {}};
    if (fed != FeedOutcome::Fed || hasher.hexDigest() != request_.expectedSha256)
      return std::nullopt;
    present_ = size;
    return Outcome{.status = DownloadStatus::Done, .failure = DownloadFailure::None, .detail = {}};
  }

  std::optional<Outcome> openPart()
  {
    std::error_code error;
    const auto parent = request_.target.parent_path();
    if (!parent.empty())
      std::filesystem::create_directories(parent, error);
    if (error)
      return filesystemFailure(error.message());
    auto opened = details::PartFile::open(part_);
    if (opened.busy)
      return Outcome{.status = DownloadStatus::Failed,
                     .failure = DownloadFailure::Busy,
                     .detail = "another download holds " + part_.string()};
    if (!opened.file.isOpen())
      return filesystemFailure(opened.error);
    file_ = std::move(opened.file);
    return std::nullopt;
  }

  std::optional<Outcome> restore()
  {
    const auto recorded = details::readSidecar(sidecar_);
    const auto partSize = file_.size();
    if (!partSize)
      return filesystemFailure(details::lastSystemError());
    const bool matches = recorded && recorded->url == request_.url &&
                         recorded->size == request_.expectedSize &&
                         recorded->sha256 == request_.expectedSha256 &&
                         recorded->bytes <= request_.expectedSize && recorded->bytes <= *partSize;
    if (matches)
    {
      present_ = recorded->bytes;
      finalUrl_ = recorded->finalUrl;
      etag_ = recorded->etag;
    }
    if (!file_.truncate(present_))
      return filesystemFailure(details::lastSystemError());
    const auto fed = feedFile({.hasher = hasher_,
                               .path = part_,
                               .length = present_,
                               .cancellation = request_.cancellation});
    if (fed == FeedOutcome::Cancelled)
      return Outcome{.status = DownloadStatus::Cancelled, .failure = DownloadFailure::None, .detail = {}};
    if (fed == FeedOutcome::Failed)
      return filesystemFailure("cannot read " + part_.string());
    if (!saveSidecar())
      return filesystemFailure("cannot write " + sidecar_.string());
    return std::nullopt;
  }

  std::optional<Outcome> fetchAll()
  {
    int failures = 0;
    while (present_ < request_.expectedSize)
    {
      if (request_.cancellation.cancelled())
        return cancelledStep().outcome;
      auto step = fetchNext();
      switch (step.kind)
      {
      case StepKind::Advanced:
        failures = 0;
        reResolves_ = 0;
        break;
      case StepKind::Again:
        break;
      case StepKind::Retry:
        if (++failures >= request_.policy.maxAttempts)
          return std::move(step.outcome);
        if (!backoff(failures))
          return cancelledStep().outcome;
        break;
      case StepKind::Stop:
        return std::move(step.outcome);
      }
    }
    return std::nullopt;
  }

  Step fetchNext()
  {
    const auto last = std::min(present_ + request_.policy.chunkBytes, request_.expectedSize) - 1;
    std::string url = finalUrl_.empty() ? request_.url : finalUrl_;
    for (int hop = 0;; ++hop)
    {
      auto response = transport_.fetch({.url = url,
                                        .first = present_,
                                        .last = last,
                                        .timeout = request_.policy.requestTimeout,
                                        .cancellation = request_.cancellation});
      if (auto failed = transportFailure(response))
        return *std::move(failed);
      httpStatus_ = response.status;
      if (!isRedirect(response.status))
        return classify({.url = std::move(url), .last = last}, response);
      if (hop >= request_.policy.maxRedirects)
        return stop(DownloadFailure::RedirectLoop, "more than " +
                                                       std::to_string(request_.policy.maxRedirects) +
                                                       " redirects from " + request_.url);
      auto next = details::resolveLocation({.base = url, .location = response.location});
      if (!next)
        return stop(DownloadFailure::HttpStatus, "redirect without a usable location");
      url = *std::move(next);
    }
  }

  static std::optional<Step> transportFailure(const ChunkResponse& response)
  {
    switch (response.error)
    {
    case TransportError::None:
      return std::nullopt;
    case TransportError::Cancelled:
      return cancelledStep();
    case TransportError::Tls:
      return stop(DownloadFailure::Tls, response.detail);
    case TransportError::InvalidUrl:
      return stop(DownloadFailure::InvalidRequest, response.detail);
    case TransportError::Network:
    case TransportError::Timeout:
      return retry(DownloadFailure::Network, response.detail);
    }
    return retry(DownloadFailure::Network, response.detail);
  }

  Step classify(const RangeTarget& target, ChunkResponse& response)
  {
    if (response.status == kPartialContent)
      return acceptPartial(target, response);
    if (response.status == kOk)
      return acceptWhole(target, response);
    const auto status = "http " + std::to_string(response.status) + " from " + target.url;
    if ((response.status == 403 || response.status == 404 || response.status == 410) &&
        target.url != request_.url)
    {
      if (++reResolves_ > request_.policy.maxReResolves)
        return stop(DownloadFailure::HttpStatus, status);
      finalUrl_.clear();
      return again();
    }
    if (response.status == 416)
    {
      discard();
      return stop(DownloadFailure::SizeMismatch, "the server's file is shorter than the pin");
    }
    if (isTransientStatus(response.status))
      return retry(DownloadFailure::HttpStatus, status);
    return stop(DownloadFailure::HttpStatus, status);
  }

  Step acceptPartial(const RangeTarget& target, const ChunkResponse& response)
  {
    const auto range = details::parseContentRange(response.contentRange);
    if (!range)
      return stop(DownloadFailure::RangeMismatch, "206 without a usable Content-Range");
    if (range->total && *range->total != request_.expectedSize)
    {
      discard();
      return stop(DownloadFailure::SizeMismatch, "the server's file is " +
                                                     std::to_string(*range->total) + " bytes, the pin " +
                                                     std::to_string(request_.expectedSize));
    }
    if (range->first != present_ || range->last > target.last ||
        response.body.size() != range->last - range->first + 1)
      return retry(DownloadFailure::RangeMismatch, "206 answered " + response.contentRange);
    if (!etag_.empty() && !response.etag.empty() && response.etag != etag_)
      return restartForNewSource(response.etag);
    if (etag_.empty())
      etag_ = response.etag;
    return append({.url = target.url, .body = response.body});
  }

  Step acceptWhole(const RangeTarget& target, const ChunkResponse& response)
  {
    if (request_.expectedSize > request_.policy.singleRequestLimit)
      return stop(DownloadFailure::RangeUnsupported,
                  target.url + " ignores Range and the file exceeds the single-request limit");
    if (response.body.size() > request_.expectedSize)
    {
      discard();
      return stop(DownloadFailure::Oversize, "the body exceeds the pinned size");
    }
    if (response.body.size() < request_.expectedSize)
    {
      discard();
      return stop(DownloadFailure::SizeMismatch, "the body is shorter than the pinned size");
    }
    if (!file_.truncate(0))
      return stop(DownloadFailure::Filesystem, details::lastSystemError());
    present_ = 0;
    hasher_.reset();
    etag_ = response.etag;
    return append({.url = target.url, .body = response.body});
  }

  struct AppendInput
  {
    const std::string& url;
    const std::string& body;
  };

  Step append(const AppendInput& input)
  {
    if (!file_.append(input.body) || !file_.sync())
      return stop(DownloadFailure::Filesystem, details::lastSystemError());
    hasher_.update(input.body);
    present_ += input.body.size();
    fetched_ += input.body.size();
    finalUrl_ = input.url == request_.url ? std::string{} : input.url;
    if (!saveSidecar())
      return stop(DownloadFailure::Filesystem, "cannot write " + sidecar_.string());
    report();
    return advanced();
  }

  Step restartForNewSource(const std::string& etag)
  {
    if (++restarts_ > request_.policy.maxAttempts)
      return stop(DownloadFailure::SourceChanged, "the source keeps changing its ETag");
    if (!file_.truncate(0))
      return stop(DownloadFailure::Filesystem, details::lastSystemError());
    present_ = 0;
    hasher_.reset();
    etag_ = etag;
    if (!saveSidecar())
      return stop(DownloadFailure::Filesystem, "cannot write " + sidecar_.string());
    report();
    return again();
  }

  Outcome commit()
  {
    if (hasher_.hexDigest() != request_.expectedSha256)
    {
      discard();
      return {.status = DownloadStatus::Failed,
              .failure = DownloadFailure::HashMismatch,
              .detail = "sha256 of " + request_.url + " differs from the pin"};
    }
    if (!file_.sync())
      return filesystemOutcome(details::lastSystemError());
    std::error_code error;
    std::filesystem::rename(part_, request_.target, error);
    if (error)
      return filesystemOutcome(error.message());
    file_.close();
    const auto parent = request_.target.parent_path();
    if (!details::syncDirectory(parent.empty() ? std::filesystem::path(".") : parent))
      return filesystemOutcome(details::lastSystemError());
    removeSidecar();
    return {.status = DownloadStatus::Done, .failure = DownloadFailure::None, .detail = {}};
  }

  [[nodiscard]] bool backoff(int failures) const
  {
    const auto& policy = request_.policy;
    auto delay = policy.backoffInitial;
    for (int i = 1; i < failures && delay < policy.backoffMax; ++i)
      delay *= 2;
    const auto deadline = std::chrono::steady_clock::now() + std::min(delay, policy.backoffMax);
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (request_.cancellation.cancelled())
        return false;
      std::this_thread::sleep_for(
          std::min<std::chrono::steady_clock::duration>(kBackoffPoll, deadline - std::chrono::steady_clock::now()));
    }
    return !request_.cancellation.cancelled();
  }

  void discard()
  {
    file_.close();
    std::error_code error;
    std::filesystem::remove(part_, error);
    removeSidecar();
    present_ = 0;
  }

  void removeSidecar() const
  {
    std::error_code error;
    std::filesystem::remove(sidecar_, error);
    auto temporary = sidecar_;
    temporary += ".tmp";
    std::filesystem::remove(temporary, error);
  }

  [[nodiscard]] bool saveSidecar() const
  {
    return details::writeSidecar(sidecar_, {.url = request_.url,
                                            .finalUrl = finalUrl_,
                                            .size = request_.expectedSize,
                                            .sha256 = request_.expectedSha256,
                                            .etag = etag_,
                                            .bytes = present_});
  }

  void report() const
  {
    if (request_.onProgress)
      request_.onProgress({.bytesPresent = present_, .bytesTotal = request_.expectedSize});
  }

  static Outcome filesystemOutcome(std::string detail)
  {
    return {.status = DownloadStatus::Failed,
            .failure = DownloadFailure::Filesystem,
            .detail = std::move(detail)};
  }

  static std::optional<Outcome> filesystemFailure(std::string detail)
  {
    return filesystemOutcome(std::move(detail));
  }

  [[nodiscard]] DownloadResult finish(Outcome outcome) const
  {
    return {.status = outcome.status,
            .failure = outcome.failure,
            .bytesPresent = present_,
            .bytesFetched = fetched_,
            .httpStatus = httpStatus_,
            .detail = std::move(outcome.detail)};
  }

  const DownloadRequest& request_;
  ChunkTransport& transport_;
  std::filesystem::path part_;
  std::filesystem::path sidecar_;
  details::PartFile file_;
  details::Sha256Stream hasher_;
  std::uint64_t present_ = 0;
  std::uint64_t fetched_ = 0;
  std::string finalUrl_;
  std::string etag_;
  int httpStatus_ = 0;
  int reResolves_ = 0;
  int restarts_ = 0;
};

}

std::string_view downloadFailureToString(DownloadFailure failure)
{
  switch (failure)
  {
  case DownloadFailure::None:
    return "none";
  case DownloadFailure::InvalidRequest:
    return "invalid_request";
  case DownloadFailure::OnEventLoop:
    return "on_event_loop";
  case DownloadFailure::Busy:
    return "busy";
  case DownloadFailure::Filesystem:
    return "filesystem";
  case DownloadFailure::Network:
    return "network";
  case DownloadFailure::Tls:
    return "tls";
  case DownloadFailure::HttpStatus:
    return "http_status";
  case DownloadFailure::RedirectLoop:
    return "redirect_loop";
  case DownloadFailure::RangeUnsupported:
    return "range_unsupported";
  case DownloadFailure::RangeMismatch:
    return "range_mismatch";
  case DownloadFailure::SourceChanged:
    return "source_changed";
  case DownloadFailure::SizeMismatch:
    return "size_mismatch";
  case DownloadFailure::Oversize:
    return "oversize";
  case DownloadFailure::HashMismatch:
    return "hash_mismatch";
  }
  return "unknown";
}

std::filesystem::path partPath(const std::filesystem::path& target)
{
  auto path = target;
  path += ".part";
  return path;
}

std::filesystem::path sidecarPath(const std::filesystem::path& target)
{
  auto path = target;
  path += ".part.json";
  return path;
}

DownloadResult downloadFile(const DownloadRequest& request)
{
  if (auto* const loop = trantor::EventLoop::getEventLoopOfCurrentThread();
      loop != nullptr && loop->isRunning())
    return {.status = DownloadStatus::Failed,
            .failure = DownloadFailure::OnEventLoop,
            .bytesPresent = 0,
            .bytesFetched = 0,
            .httpStatus = 0,
            .detail = "downloadFile blocks; call it off every event loop"};
  auto transport = request.transport;
  if (!transport)
    transport = std::make_shared<DrogonChunkTransport>();
  return Transfer(request, *transport).run();
}

std::uint64_t presentBytes(const std::filesystem::path& target)
{
  std::error_code error;
  if (const auto size = std::filesystem::file_size(target, error); !error)
    return size;
  const auto recorded = details::readSidecar(sidecarPath(target));
  const auto partSize = std::filesystem::file_size(partPath(target), error);
  if (!recorded || error)
    return 0;
  return std::min(recorded->bytes, partSize);
}

}
