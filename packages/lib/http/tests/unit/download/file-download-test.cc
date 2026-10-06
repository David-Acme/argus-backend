#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-transport.hxx"
#include "range-server.hxx"

#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <http/download/details/sha256-stream.hxx>
#include <http/download/file-download.hxx>
#include <iterator>
#include <random>
#include <sys/file.h>
#include <trantor/net/EventLoopThread.h>
#include <semaphore>
#include <unistd.h>

using namespace std::chrono_literals;
using file_download::ChunkResponse;
using file_download::DownloadFailure;
using file_download::DownloadPolicy;
using file_download::DownloadRequest;
using file_download::DownloadResult;
using file_download::DownloadStatus;
using file_download::TransportError;

namespace
{

constexpr std::uint64_t kChunk = std::uint64_t{64} * 1024;

struct ContentSpec
{
  std::uint64_t size = 0;
  unsigned seed = 0;
};

std::string makeContent(const ContentSpec& spec)
{
  std::mt19937 generator(spec.seed);
  std::uniform_int_distribution<int> byte(0, 255);
  std::string content(spec.size, '\0');
  for (auto& c : content)
    c = static_cast<char>(byte(generator));
  return content;
}

std::string sha256Of(std::string_view bytes)
{
  file_download::details::Sha256Stream hasher;
  hasher.update(bytes);
  return hasher.hexDigest();
}

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, std::string_view bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

class TempDir
{
public:
  TempDir()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "argus-download-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr)
      throw std::runtime_error("mkdtemp");
    path_ = pattern;
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir()
  {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path target() const { return path_ / "models" / "model.bin"; }

private:
  std::filesystem::path path_;
};

DownloadPolicy fastPolicy()
{
  return {.chunkBytes = kChunk,
          .singleRequestLimit = std::uint64_t{1} << 20U,
          .requestTimeout = 2000ms,
          .maxRedirects = 4,
          .maxReResolves = 2,
          .maxAttempts = 3,
          .backoffInitial = 1ms,
          .backoffMax = 5ms};
}

struct Pin
{
  std::string url;
  std::filesystem::path target;
  const std::string& content;
};

DownloadRequest requestFor(const Pin& pin)
{
  return {.url = pin.url,
          .target = pin.target,
          .expectedSize = pin.content.size(),
          .expectedSha256 = sha256Of(pin.content),
          .cancellation = {},
          .onProgress = {},
          .policy = fastPolicy(),
          .transport = nullptr};
}

void requireNothingLeft(const std::filesystem::path& target)
{
  CHECK_FALSE(std::filesystem::exists(target));
  CHECK_FALSE(std::filesystem::exists(file_download::partPath(target)));
  CHECK_FALSE(std::filesystem::exists(file_download::sidecarPath(target)));
}

void requireDone(const DownloadResult& result, const Pin& pin)
{
  INFO("failure: " << file_download::downloadFailureToString(result.failure) << " " << result.detail);
  REQUIRE(result.status == DownloadStatus::Done);
  CHECK(result.bytesPresent == pin.content.size());
  CHECK(readFile(pin.target) == pin.content);
  CHECK_FALSE(std::filesystem::exists(file_download::partPath(pin.target)));
  CHECK_FALSE(std::filesystem::exists(file_download::sidecarPath(pin.target)));
  CHECK(file_download::presentBytes(pin.target) == pin.content.size());
}

std::shared_ptr<FakeTransport> fakeOver(const std::string& content)
{
  return std::make_shared<FakeTransport>(content);
}

}

TEST_CASE("fake: a file arrives chunk by chunk, verified and renamed")
{
  TempDir dir;
  const auto content = makeContent({.size = 5 * kChunk + 123, .seed = 1});
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  std::vector<std::uint64_t> progress;
  request.onProgress = [&progress](const file_download::DownloadProgress& p)
  {
    CHECK(p.bytesTotal == 5 * kChunk + 123);
    progress.push_back(p.bytesPresent);
  };
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(result.bytesFetched == content.size());
  CHECK(fake->calls().size() == 6);
  REQUIRE(progress.size() == 7);
  CHECK(progress.front() == 0);
  CHECK(std::ranges::is_sorted(progress));
  CHECK(progress.back() == content.size());
}

TEST_CASE("fake: a network cut mid-chunk refetches only that chunk")
{
  TempDir dir;
  const auto content = makeContent({.size = 4 * kChunk, .seed = 2});
  auto fake = fakeOver(content);
  fake->intercept = [](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index == 2)
      return FakeTransport::error(TransportError::Network);
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  const auto calls = fake->calls();
  REQUIRE(calls.size() == 5);
  CHECK(calls[2].first == 2 * kChunk);
  CHECK(calls[3].first == 2 * kChunk);
  CHECK(calls[4].first == 3 * kChunk);
  CHECK(result.bytesFetched == content.size());
}

TEST_CASE("fake: repeated failures stop after the attempt budget and keep the partial")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 3});
  auto fake = fakeOver(content);
  fake->intercept = [](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index >= 1)
      return FakeTransport::error(TransportError::Timeout);
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  CHECK(result.status == DownloadStatus::Failed);
  CHECK(result.failure == DownloadFailure::Network);
  CHECK(fake->calls().size() == 1 + 3);
  CHECK(result.bytesPresent == kChunk);
  CHECK_FALSE(std::filesystem::exists(dir.target()));
  CHECK(file_download::presentBytes(dir.target()) == kChunk);

  fake->intercept = nullptr;
  const auto resumed = file_download::downloadFile(request);
  requireDone(resumed, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(resumed.bytesFetched == 2 * kChunk);
}

TEST_CASE("fake: transient statuses are retried, a client error is not")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 4});
  auto fake = fakeOver(content);
  fake->intercept = [](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index == 0)
      return FakeTransport::status(503);
    if (call.index == 1)
      return FakeTransport::status(429);
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});

  TempDir other;
  auto refused = fakeOver(content);
  refused->intercept = [](const FakeCall&) -> std::optional<ChunkResponse>
  { return FakeTransport::status(401); };
  request.target = other.target();
  request.transport = refused;
  const auto result = file_download::downloadFile(request);
  CHECK(result.failure == DownloadFailure::HttpStatus);
  CHECK(result.httpStatus == 401);
  CHECK(refused->calls().size() == 1);
}

TEST_CASE("fake: the hub redirect is followed and the CDN url is reused per chunk")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 5});
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kHubUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  const auto calls = fake->calls();
  REQUIRE(calls.size() == 4);
  CHECK(calls[0].url == FakeTransport::kHubUrl);
  CHECK(calls[1].url.starts_with("https://cdn.test/"));
  CHECK(calls[2].url == calls[1].url);
  CHECK(calls[3].url == calls[1].url);
}

TEST_CASE("fake: an expired CDN url (403) re-resolves through the original url")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 6});
  auto fake = fakeOver(content);
  auto* raw = fake.get();
  fake->intercept = [raw](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index == 2)
      raw->expireCdn();
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kHubUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  const auto calls = fake->calls();
  const auto hubCalls = std::ranges::count_if(calls, [](const FakeCall& c) { return c.url == FakeTransport::kHubUrl; });
  CHECK(hubCalls == 2);
  CHECK(calls.back().url.ends_with("gen=2"));
  CHECK(result.bytesFetched == content.size());
}

TEST_CASE("fake: a CDN that keeps refusing exhausts the re-resolve budget")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 7});
  auto fake = fakeOver(content);
  fake->intercept = [](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.url.starts_with("https://cdn.test/"))
      return FakeTransport::status(403);
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kHubUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  CHECK(result.failure == DownloadFailure::HttpStatus);
  CHECK(result.httpStatus == 403);
  CHECK(fake->calls().size() == 2 * 3);
}

TEST_CASE("fake: a cancelled download resumes from its sidecar, cached CDN url included")
{
  TempDir dir;
  const auto content = makeContent({.size = 4 * kChunk, .seed = 8});
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kHubUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  CancellationToken token;
  request.cancellation = token;
  request.onProgress = [token](const file_download::DownloadProgress& p)
  {
    if (p.bytesPresent >= 2 * kChunk)
      token.cancel();
  };
  const auto cancelled = file_download::downloadFile(request);
  CHECK(cancelled.status == DownloadStatus::Cancelled);
  CHECK(cancelled.bytesPresent == 2 * kChunk);
  CHECK(file_download::presentBytes(dir.target()) == 2 * kChunk);
  const auto before = fake->calls().size();

  token.reset();
  request.onProgress = nullptr;
  const auto resumed = file_download::downloadFile(request);
  requireDone(resumed, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(resumed.bytesFetched == 2 * kChunk);
  const auto calls = fake->calls();
  REQUIRE(calls.size() == before + 2);
  CHECK(calls[before].first == 2 * kChunk);
  CHECK(calls[before].url.starts_with("https://cdn.test/"));
}

TEST_CASE("fake: a changed ETag restarts from zero")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 9});
  auto fake = fakeOver(content);
  auto* raw = fake.get();
  fake->intercept = [raw](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index == 1)
      raw->setEtag("\"v2\"");
    return std::nullopt;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  const auto calls = fake->calls();
  REQUIRE(calls.size() == 5);
  CHECK(calls[1].first == kChunk);
  CHECK(calls[2].first == 0);
}

TEST_CASE("fake: a server ignoring Range is accepted only under the single-request limit")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 10});
  auto fake = fakeOver(content);
  fake->setIgnoreRange(true);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  CHECK(fake->calls().size() == 1);

  TempDir big;
  request.target = big.target();
  request.policy.singleRequestLimit = kChunk;
  const auto refused = file_download::downloadFile(request);
  CHECK(refused.failure == DownloadFailure::RangeUnsupported);
  CHECK_FALSE(std::filesystem::exists(big.target()));
}

TEST_CASE("fake: a Content-Range total that disagrees with the pin leaves nothing")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 11});
  auto fake = fakeOver(content);
  fake->intercept = [](const FakeCall& call) -> std::optional<ChunkResponse>
  {
    if (call.index != 1)
      return std::nullopt;
    auto response = FakeTransport::status(206);
    response.contentRange = "bytes " + std::to_string(call.first) + "-" + std::to_string(call.last) + "/" +
                            std::to_string(3 * kChunk);
    response.body = std::string(call.last - call.first + 1, 'x');
    return response;
  };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  CHECK(result.failure == DownloadFailure::SizeMismatch);
  requireNothingLeft(dir.target());
}

TEST_CASE("fake: TLS failures and redirect loops stop at once")
{
  TempDir dir;
  const auto content = makeContent({.size = kChunk, .seed = 12});
  auto tls = fakeOver(content);
  tls->intercept = [](const FakeCall&) -> std::optional<ChunkResponse>
  { return FakeTransport::error(TransportError::Tls); };
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = tls;
  CHECK(file_download::downloadFile(request).failure == DownloadFailure::Tls);
  CHECK(tls->calls().size() == 1);
  requireNothingLeft(dir.target());

  auto loop = fakeOver(content);
  loop->intercept = [](const FakeCall&) -> std::optional<ChunkResponse>
  {
    auto redirect = FakeTransport::status(302);
    redirect.location = "/again";
    return redirect;
  };
  request.transport = loop;
  CHECK(file_download::downloadFile(request).failure == DownloadFailure::RedirectLoop);
  CHECK(loop->calls().size() == 5);
}

TEST_CASE("fake: a wrong hash is reported and nothing is left behind")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk + 7, .seed = 13});
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.expectedSha256 = sha256Of("something else");
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  CHECK(result.status == DownloadStatus::Failed);
  CHECK(result.failure == DownloadFailure::HashMismatch);
  requireNothingLeft(dir.target());
  CHECK(file_download::presentBytes(dir.target()) == 0);
}

TEST_CASE("fake: a sidecar for another url or hash restarts clean")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 14});
  std::filesystem::create_directories(dir.target().parent_path());
  writeFile(file_download::partPath(dir.target()), std::string(kChunk, 'g'));
  writeFile(file_download::sidecarPath(dir.target()),
            R"({"url":"https://elsewhere.test/x","finalUrl":"","size":131072,"sha256":")" + sha256Of(content) +
                R"(","etag":"","bytes":65536})");
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  CHECK(fake->calls().front().first == 0);

  TempDir other;
  std::filesystem::create_directories(other.target().parent_path());
  writeFile(file_download::partPath(other.target()), std::string(kChunk, 'g'));
  writeFile(file_download::sidecarPath(other.target()),
            R"({"url":")" + std::string(FakeTransport::kDirectUrl) + R"(","finalUrl":"","size":131072,"sha256":")" +
                sha256Of("other") + R"(","etag":"","bytes":65536})");
  auto second = fakeOver(content);
  request.target = other.target();
  request.transport = second;
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = other.target(), .content = content});
  CHECK(second->calls().front().first == 0);
}

TEST_CASE("fake: bytes past the sidecar's confirmed count are not trusted")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 15});
  std::filesystem::create_directories(dir.target().parent_path());
  writeFile(file_download::partPath(dir.target()), content.substr(0, kChunk) + std::string(100, 'z'));
  writeFile(file_download::sidecarPath(dir.target()),
            R"({"url":")" + std::string(FakeTransport::kDirectUrl) + R"(","finalUrl":"","size":)" +
                std::to_string(content.size()) + R"(,"sha256":")" + sha256Of(content) +
                R"(","etag":"","bytes":65536})");
  CHECK(file_download::presentBytes(dir.target()) == kChunk);
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(fake->calls().front().first == kChunk);
  CHECK(result.bytesFetched == 2 * kChunk);
}

TEST_CASE("fake: a verified final file needs no request, a corrupt one is replaced")
{
  TempDir dir;
  const auto content = makeContent({.size = kChunk + 1, .seed = 16});
  std::filesystem::create_directories(dir.target().parent_path());
  writeFile(dir.target(), content);
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  const auto kept = file_download::downloadFile(request);
  requireDone(kept, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(kept.bytesFetched == 0);
  CHECK(fake->calls().empty());

  auto corrupt = content;
  corrupt[10] = static_cast<char>(static_cast<unsigned char>(corrupt[10]) ^ 0x5AU);
  writeFile(dir.target(), corrupt);
  const auto replaced = file_download::downloadFile(request);
  requireDone(replaced, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(replaced.bytesFetched == content.size());
}

TEST_CASE("fake: a target another download holds is refused as busy")
{
  TempDir dir;
  const auto content = makeContent({.size = kChunk, .seed = 17});
  std::filesystem::create_directories(dir.target().parent_path());
  const int holder = ::open(file_download::partPath(dir.target()).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  REQUIRE(holder >= 0);
  REQUIRE(::flock(holder, LOCK_EX | LOCK_NB) == 0);
  auto fake = fakeOver(content);
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fake;
  CHECK(file_download::downloadFile(request).failure == DownloadFailure::Busy);
  CHECK(fake->calls().empty());
  ::close(holder);
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
}

TEST_CASE("fake: invalid requests and calls from an event loop are refused")
{
  TempDir dir;
  const auto content = makeContent({.size = 16, .seed = 18});
  auto request = requestFor({.url = std::string(FakeTransport::kDirectUrl), .target = dir.target(), .content = content});
  request.transport = fakeOver(content);
  auto bad = request;
  bad.url = "ftp://files.test/model.bin";
  CHECK(file_download::downloadFile(bad).failure == DownloadFailure::InvalidRequest);
  bad = request;
  bad.expectedSha256 = "ABC";
  CHECK(file_download::downloadFile(bad).failure == DownloadFailure::InvalidRequest);
  bad = request;
  bad.expectedSize = 0;
  CHECK(file_download::downloadFile(bad).failure == DownloadFailure::InvalidRequest);

  trantor::EventLoopThread loop;
  loop.run();
  std::binary_semaphore finished{0};
  DownloadResult onLoop;
  loop.getLoop()->runInLoop(
      [&]
      {
        onLoop = file_download::downloadFile(request);
        finished.release();
      });
  finished.acquire();
  CHECK(onLoop.failure == DownloadFailure::OnEventLoop);
  requireNothingLeft(dir.target());
}

TEST_CASE("server: a ranged download over Drogon's client lands verified")
{
  TempDir dir;
  const auto content = makeContent({.size = 5 * kChunk + 999, .seed = 21});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  const auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = content});
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  const auto served = server.fileRequests();
  REQUIRE(served.size() == 6);
  for (std::size_t i = 0; i < served.size(); ++i)
    CHECK(served[i].rangeFirst == i * kChunk);
}

TEST_CASE("server: a connection dropped mid-chunk resumes at that chunk")
{
  TempDir dir;
  const auto content = makeContent({.size = 4 * kChunk, .seed = 22});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = 2});
  const auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = content});
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(result.bytesFetched == content.size());
  const auto served = server.fileRequests();
  REQUIRE(served.size() == 5);
  CHECK(served[2].rangeFirst == 2 * kChunk);
  CHECK(served[3].rangeFirst == 2 * kChunk);
  CHECK(served[4].rangeFirst == 3 * kChunk);
}

TEST_CASE("server: a server ignoring Range restarts the partial from the full body")
{
  TempDir dir;
  const auto content = makeContent({.size = 3 * kChunk, .seed = 23});
  std::filesystem::create_directories(dir.target().parent_path());
  RangeServer server({.content = content, .honourRange = false, .dropOnFileRequest = -1});
  const auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = content});
  writeFile(file_download::partPath(dir.target()), content.substr(0, kChunk));
  writeFile(file_download::sidecarPath(dir.target()),
            R"({"url":")" + request.url + R"(","finalUrl":"","size":)" + std::to_string(content.size()) +
                R"(,"sha256":")" + request.expectedSha256 + R"(","etag":"","bytes":65536})");
  const auto result = file_download::downloadFile(request);
  requireDone(result, {.url = request.url, .target = dir.target(), .content = content});
  const auto served = server.fileRequests();
  REQUIRE(served.size() == 1);
  CHECK(served[0].rangeFirst == kChunk);
  CHECK(result.bytesFetched == content.size());
}

TEST_CASE("server: a body larger than the pin is refused and nothing is left")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 24});
  RangeServer server({.content = content, .honourRange = false, .dropOnFileRequest = -1});
  const auto pinned = content.substr(0, content.size() - 100);
  const auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = pinned});
  const auto result = file_download::downloadFile(request);
  CHECK(result.failure == DownloadFailure::Oversize);
  requireNothingLeft(dir.target());
}

TEST_CASE("server: a wrong hash is rejected over the wire too")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk, .seed = 25});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = content});
  request.expectedSha256 = sha256Of("not this");
  CHECK(file_download::downloadFile(request).failure == DownloadFailure::HashMismatch);
  requireNothingLeft(dir.target());
}

TEST_CASE("server: relative and absolute redirects are followed")
{
  TempDir dir;
  const auto content = makeContent({.size = 2 * kChunk + 5, .seed = 26});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  const auto request = requestFor({.url = server.url("/absolute"), .target = dir.target(), .content = content});
  requireDone(file_download::downloadFile(request), {.url = request.url, .target = dir.target(), .content = content});
  const auto served = server.requests();
  REQUIRE(served.size() == 5);
  CHECK(served[0].path == "/absolute");
  CHECK(served[1].path == "/redirect");
  CHECK(served[2].path == "/file");
  CHECK(served[3].path == "/file");
  CHECK(served[4].path == "/file");

  TempDir looping;
  auto loop = requestFor({.url = server.url("/loop"), .target = looping.target(), .content = content});
  CHECK(file_download::downloadFile(loop).failure == DownloadFailure::RedirectLoop);
}

TEST_CASE("server: a cancelled download resumes with only the missing bytes")
{
  TempDir dir;
  const auto content = makeContent({.size = 4 * kChunk + 17, .seed = 27});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  auto request = requestFor({.url = server.url("/file"), .target = dir.target(), .content = content});
  CancellationToken token;
  request.cancellation = token;
  request.onProgress = [token](const file_download::DownloadProgress& p)
  {
    if (p.bytesPresent >= kChunk)
      token.cancel();
  };
  const auto cancelled = file_download::downloadFile(request);
  REQUIRE(cancelled.status == DownloadStatus::Cancelled);
  CHECK(cancelled.bytesPresent == kChunk);
  CHECK(file_download::presentBytes(dir.target()) == kChunk);
  CHECK_FALSE(std::filesystem::exists(dir.target()));

  token.reset();
  request.onProgress = nullptr;
  const auto resumed = file_download::downloadFile(request);
  requireDone(resumed, {.url = request.url, .target = dir.target(), .content = content});
  CHECK(resumed.bytesFetched == content.size() - kChunk);
  const auto served = server.fileRequests();
  REQUIRE(served.size() == 5);
  CHECK(served[1].rangeFirst == kChunk);
}

TEST_CASE("server: a cancellation during a silent request returns promptly")
{
  TempDir dir;
  const auto content = makeContent({.size = kChunk, .seed = 28});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  auto request = requestFor({.url = server.url("/silent"), .target = dir.target(), .content = content});
  request.policy.requestTimeout = 30s;
  CancellationToken token;
  request.cancellation = token;
  std::atomic<bool> returned{false};
  std::thread canceller(
      [&server, &returned, token]
      {
        while (server.requests().empty() && !returned.load())
          std::this_thread::yield();
        token.cancel();
      });
  const auto started = std::chrono::steady_clock::now();
  const auto result = file_download::downloadFile(request);
  returned.store(true);
  canceller.join();
  CHECK(result.status == DownloadStatus::Cancelled);
  CHECK(std::chrono::steady_clock::now() - started < 10s);
}

TEST_CASE("server: a silent server times out within the attempt budget")
{
  TempDir dir;
  const auto content = makeContent({.size = kChunk, .seed = 29});
  RangeServer server({.content = content, .honourRange = true, .dropOnFileRequest = -1});
  auto request = requestFor({.url = server.url("/silent"), .target = dir.target(), .content = content});
  request.policy.requestTimeout = 200ms;
  request.policy.maxAttempts = 2;
  const auto result = file_download::downloadFile(request);
  CHECK(result.failure == DownloadFailure::Network);
  CHECK(server.requests().size() == 2);
}
