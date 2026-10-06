#include "fake-transport.hxx"

#include <algorithm>

using file_download::ChunkRequest;
using file_download::ChunkResponse;
using file_download::TransportError;

namespace
{

constexpr std::string_view kCdnPrefix = "https://cdn.test/model.bin?gen=";

}

ChunkResponse FakeTransport::status(int code)
{
  return {.error = TransportError::None,
          .status = code,
          .contentRange = {},
          .etag = {},
          .location = {},
          .body = {},
          .detail = {}};
}

ChunkResponse FakeTransport::error(TransportError kind)
{
  return {.error = kind,
          .status = 0,
          .contentRange = {},
          .etag = {},
          .location = {},
          .body = {},
          .detail = "fake transport error"};
}

ChunkResponse FakeTransport::fetch(const ChunkRequest& request)
{
  if (request.cancellation.cancelled())
    return error(TransportError::Cancelled);
  FakeCall call;
  {
    const std::scoped_lock lock(mutex_);
    call = {.index = static_cast<int>(calls_.size()),
            .url = request.url,
            .first = request.first,
            .last = request.last};
    calls_.push_back(call);
  }
  if (intercept)
    if (auto replaced = intercept(call))
      return *std::move(replaced);
  return serve(call);
}

ChunkResponse FakeTransport::serve(const FakeCall& call) const
{
  const std::scoped_lock lock(mutex_);
  if (call.url == kHubUrl)
  {
    auto redirect = status(302);
    redirect.location = std::string(kCdnPrefix) + std::to_string(generation_);
    return redirect;
  }
  const bool cdn = call.url.starts_with(kCdnPrefix);
  if (cdn && call.url != std::string(kCdnPrefix) + std::to_string(generation_))
    return status(403);
  if (!cdn && call.url != kDirectUrl)
    return status(404);
  if (ignoreRange_)
  {
    auto whole = status(200);
    whole.etag = etag_;
    whole.body = content_;
    return whole;
  }
  const auto last = std::min<std::uint64_t>(call.last, content_.size() - 1);
  auto partial = status(206);
  partial.etag = etag_;
  partial.contentRange = "bytes " + std::to_string(call.first) + "-" + std::to_string(last) + "/" +
                         std::to_string(content_.size());
  partial.body = content_.substr(call.first, last - call.first + 1);
  return partial;
}

void FakeTransport::expireCdn()
{
  const std::scoped_lock lock(mutex_);
  ++generation_;
}

void FakeTransport::setEtag(std::string etag)
{
  const std::scoped_lock lock(mutex_);
  etag_ = std::move(etag);
}

void FakeTransport::setIgnoreRange(bool ignore)
{
  const std::scoped_lock lock(mutex_);
  ignoreRange_ = ignore;
}

std::vector<FakeCall> FakeTransport::calls() const
{
  const std::scoped_lock lock(mutex_);
  return calls_;
}
