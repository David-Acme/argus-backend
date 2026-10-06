#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/vlm-config.hxx>
#include <errors/response-exception.hxx>
#include <feature/components/infra/vision-fetch.hxx>
#include <feature/components/services/vision-component-host.hxx>
#include <feature/settings/vlm-settings.hxx>
#include <feature/vlm/services/vision-service.hxx>
#include <http/download/details/sha256-stream.hxx>
#include <http/download/drogon-chunk-transport.hxx>
#include <settings/component-wire.hxx>
#include <settings/settings-rpc.hxx>

#include <range-server.hxx>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace
{
namespace fs = std::filesystem;
namespace wire = argus::settings::v1;

constexpr const char* kSettingsSecret = "vlm-components-test-settings";
constexpr std::string_view kModelUrl = "https://models.test/LFM2.5-VL-450M-Q8_0.gguf";
constexpr std::string_view kMmprojUrl = "https://models.test/mmproj-LFM2.5-VL-450m-F16.gguf";
constexpr std::uint64_t kChunk = std::uint64_t{64} << 10U;

struct TempDir
{
  fs::path path;

  TempDir()
      : path(fs::temp_directory_path() /
             ("argus-vlm-components-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
  {
    fs::create_directories(path);
  }

  ~TempDir()
  {
    std::error_code error;
    fs::remove_all(path, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((fs::temp_directory_path() / "vlm-components-test.toml").string())
  {
    std::ofstream(path_) << content;
    ConfigService::load(path_);
  }

  ~ScopedConfig()
  {
    std::ofstream(path_).flush();
    ConfigService::load(path_);
    std::remove(path_.c_str());
  }

  ScopedConfig(const ScopedConfig&) = delete;
  ScopedConfig& operator=(const ScopedConfig&) = delete;

private:
  std::string path_;
};

struct Pattern
{
  std::size_t size{0};
  char seed{'a'};
};

std::string patterned(const Pattern& pattern)
{
  std::string content(pattern.size, '\0');
  for (std::size_t index = 0; index < pattern.size; ++index)
    content[index] = static_cast<char>(pattern.seed + static_cast<char>(index % 97U));
  return content;
}

std::string sha256Of(std::string_view bytes)
{
  file_download::details::Sha256Stream hasher;
  hasher.update(bytes);
  return hasher.hexDigest();
}

std::string readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Predicate>
bool eventually(const Predicate& predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::yield();
  }
  return predicate();
}

struct Weights
{
  std::string model = patterned({.size = static_cast<std::size_t>(3 * kChunk + 1234), .seed = 'a'});
  std::string mmproj = patterned({.size = static_cast<std::size_t>(kChunk + 77), .seed = 'k'});
};

ComponentSpec visionSpec(const Weights& weights)
{
  return {.id = "vision",
          .source = ComponentSource::Download,
          .files = {{.path = "vision/lfm2vl-25/lm-Q8_0.gguf",
                     .url = std::string(kModelUrl),
                     .sizeBytes = static_cast<std::int64_t>(weights.model.size()),
                     .sha256 = sha256Of(weights.model)},
                    {.path = "vision/lfm2vl-25/mmproj-F16.gguf",
                     .url = std::string(kMmprojUrl),
                     .sizeBytes = static_cast<std::int64_t>(weights.mmproj.size()),
                     .sha256 = sha256Of(weights.mmproj)}},
          .hostCommand = {}};
}

class LocalTransport final : public file_download::ChunkTransport
{
public:
  LocalTransport(const RangeServer& model, const RangeServer& mmproj)
      : model_(model.url("/file")), mmproj_(mmproj.url("/file"))
  {
  }

  [[nodiscard]] file_download::ChunkResponse fetch(const file_download::ChunkRequest& request) override
  {
    if (request.first > 0)
      waitForRelease(request.cancellation);
    auto local = request;
    local.url = request.url == kModelUrl ? model_ : mmproj_;
    return inner_.fetch(local);
  }

  void hold()
  {
    const std::scoped_lock lock(mutex_);
    held_ = true;
  }

  void release()
  {
    {
      const std::scoped_lock lock(mutex_);
      held_ = false;
    }
    released_.notify_all();
  }

  [[nodiscard]] bool waiting() const { return waiting_.load(); }

private:
  void waitForRelease(const CancellationToken& cancellation)
  {
    std::unique_lock lock(mutex_);
    waiting_ = held_;
    while (held_ && !cancellation.cancelled())
      released_.wait_for(lock, std::chrono::milliseconds(5));
    waiting_ = false;
  }

  std::string model_;
  std::string mmproj_;
  file_download::DrogonChunkTransport inner_;
  std::mutex mutex_;
  std::condition_variable released_;
  bool held_{false};
  std::atomic<bool> waiting_{false};
};

struct FakeEngine
{
  std::atomic<bool> loaded{false};
  std::atomic<int> loads{0};
  std::atomic<int> unloads{0};
  std::atomic<bool> loadSucceeds{true};

  VisionComponentHostInput hostInput(const fs::path& models, ComponentFetch fetch)
  {
    return {.modelsDir = models,
            .fetch = std::move(fetch),
            .loaded = [this] { return loaded.load(); },
            .load =
                [this] {
                  ++loads;
                  loaded = loadSucceeds.load();
                },
            .unload =
                [this] {
                  ++unloads;
                  loaded = false;
                }};
  }
};

struct Fixture
{
  Weights weights;
  RangeServer modelServer{{.content = weights.model, .honourRange = true, .dropOnFileRequest = -1}};
  RangeServer mmprojServer{{.content = weights.mmproj, .honourRange = true, .dropOnFileRequest = -1}};
  std::shared_ptr<LocalTransport> transport = std::make_shared<LocalTransport>(modelServer, mmprojServer);
  TempDir models;
  FakeEngine engine;

  ComponentFetch fetch()
  {
    file_download::DownloadPolicy policy;
    policy.chunkBytes = kChunk;
    policy.backoffInitial = std::chrono::milliseconds(10);
    policy.backoffMax = std::chrono::milliseconds(20);
    return visionFetch({.transport = transport, .policy = policy});
  }
};
}

TEST_CASE("the vision component downloads from a local server, verifies, renames and loads once")
{
  Fixture fixture;
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  const auto spec = visionSpec(fixture.weights);

  const auto missing = host.status(spec);
  CHECK(missing.state == ComponentState::Missing);
  CHECK(missing.bytesPresent == 0);
  CHECK(missing.bytesTotal == spec.totalBytes());
  CHECK_FALSE(missing.ready);

  fixture.transport->hold();
  CHECK(host.install(spec).state == ComponentState::Installing);
  REQUIRE(eventually([&] { return fixture.transport->waiting(); }));
  const auto installing = host.status(spec);
  CHECK(installing.state == ComponentState::Installing);
  CHECK(installing.bytesPresent == static_cast<std::int64_t>(kChunk));
  CHECK_FALSE(installing.ready);

  fixture.transport->release();
  REQUIRE(eventually([&] { return host.status(spec).state == ComponentState::Installed; }));
  CHECK(readFile(fixture.models.path / "vision/lfm2vl-25/lm-Q8_0.gguf") == fixture.weights.model);
  CHECK(readFile(fixture.models.path / "vision/lfm2vl-25/mmproj-F16.gguf") == fixture.weights.mmproj);
  CHECK_FALSE(fs::exists(fixture.models.path / "vision/lfm2vl-25/lm-Q8_0.gguf.part"));

  CHECK(eventually([&] { return host.status(spec).ready; }));
  CHECK(fixture.engine.loads.load() == 1);
  const auto installed = host.status(spec);
  CHECK(installed.bytesPresent == spec.totalBytes());
  CHECK(fixture.engine.loads.load() == 1);
}

TEST_CASE("cancel keeps the partial file, a second install resumes it, remove unloads and deletes")
{
  Fixture fixture;
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  const auto spec = visionSpec(fixture.weights);

  fixture.transport->hold();
  host.install(spec);
  REQUIRE(eventually([&] { return fixture.transport->waiting(); }));
  const auto cancelled = host.cancel(spec);
  CHECK(cancelled.state == ComponentState::Missing);
  CHECK(cancelled.bytesPresent == static_cast<std::int64_t>(kChunk));
  CHECK(cancelled.reason.empty());
  const auto before = fixture.modelServer.fileRequests().size();

  fixture.transport->release();
  host.install(spec);
  REQUIRE(eventually([&] { return host.status(spec).state == ComponentState::Installed; }));
  const auto served = fixture.modelServer.fileRequests();
  REQUIRE(served.size() > before);
  for (auto request = served.begin() + static_cast<std::ptrdiff_t>(before); request != served.end(); ++request)
    CHECK(request->rangeFirst.value_or(0) >= kChunk);
  REQUIRE(eventually([&] { return host.status(spec).ready; }));

  const auto removed = host.remove(spec);
  CHECK(removed.state == ComponentState::Missing);
  CHECK(removed.bytesPresent == 0);
  CHECK(fixture.engine.unloads.load() == 1);
  CHECK_FALSE(fixture.engine.loaded.load());
  CHECK_FALSE(fs::exists(fixture.models.path / "vision/lfm2vl-25/lm-Q8_0.gguf"));
  CHECK_FALSE(fs::exists(fixture.models.path / "vision/lfm2vl-25/mmproj-F16.gguf"));

  host.install(spec);
  REQUIRE(eventually([&] { return host.status(spec).ready; }));
  CHECK(fixture.engine.loads.load() == 2);
}

TEST_CASE("a pinned hash the bytes do not match fails as checksum_mismatch and installs nothing")
{
  Fixture fixture;
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  auto spec = visionSpec(fixture.weights);
  spec.files.front().sha256 = sha256Of("not the model");

  host.install(spec);
  REQUIRE(eventually([&] { return host.status(spec).state == ComponentState::Failed; }));
  const auto failed = host.status(spec);
  CHECK(failed.reason == "checksum_mismatch");
  CHECK_FALSE(fs::exists(fixture.models.path / "vision/lfm2vl-25/lm-Q8_0.gguf"));
  CHECK(fixture.engine.loads.load() == 0);
}

TEST_CASE("an engine that does not load is tried once per install and never reported ready")
{
  Fixture fixture;
  fixture.engine.loadSucceeds = false;
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  const auto spec = visionSpec(fixture.weights);

  host.install(spec);
  REQUIRE(eventually([&] { return host.status(spec).state == ComponentState::Installed; }));
  REQUIRE(eventually([&] { return fixture.engine.loads.load() == 1; }));
  CHECK_FALSE(host.status(spec).ready);
  CHECK_FALSE(host.status(spec).ready);

  fixture.engine.loadSucceeds = true;
  host.install(spec);
  CHECK(eventually([&] { return host.status(spec).ready; }));
  CHECK(fixture.engine.loads.load() == 2);
}

TEST_CASE("a component that is not vision, or not pinned, is refused before anything is touched")
{
  Fixture fixture;
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  auto foreign = visionSpec(fixture.weights);
  foreign.id = "detector";
  CHECK_THROWS_AS(static_cast<void>(host.status(foreign)), std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(host.remove(foreign)), std::invalid_argument);
  auto unpinned = visionSpec(fixture.weights);
  unpinned.files.front().sha256.clear();
  CHECK_THROWS_AS(static_cast<void>(host.install(unpinned)), std::invalid_argument);
  CHECK(fixture.engine.unloads.load() == 0);
}

TEST_CASE("download failures map to the reason codes argus-settings passes to the app")
{
  using file_download::DownloadFailure;
  TempDir models;
  const auto target = models.path / "lm.gguf";
  CHECK(fetchFailureReason({.failure = DownloadFailure::Network, .target = target, .remainingBytes = 1}) ==
        "network");
  CHECK(fetchFailureReason({.failure = DownloadFailure::HashMismatch, .target = target, .remainingBytes = 1}) ==
        "checksum_mismatch");
  CHECK(fetchFailureReason({.failure = DownloadFailure::SizeMismatch, .target = target, .remainingBytes = 1}) ==
        "checksum_mismatch");
  CHECK(fetchFailureReason({.failure = DownloadFailure::HttpStatus, .target = target, .remainingBytes = 1}) ==
        "source_unavailable");
  CHECK(fetchFailureReason({.failure = DownloadFailure::Tls, .target = target, .remainingBytes = 1}) ==
        "source_unavailable");
  CHECK(fetchFailureReason({.failure = DownloadFailure::Filesystem,
                            .target = target,
                            .remainingBytes = std::numeric_limits<std::int64_t>::max()}) == "disk_full");
  CHECK(fetchFailureReason({.failure = DownloadFailure::Filesystem, .target = target, .remainingBytes = 1}) ==
        "filesystem");
}

TEST_CASE("the settings wire answers the vision component's states through the host")
{
  Fixture fixture;
  const ScopedConfig config("[vision]\nmax_tokens = 64\n");
  SettingsRegistry registry(vlmSettingsCatalog());
  SettingsRpcService service({.service = "vlm",
                              .registry = &registry,
                              .credentials = {{.service = "settings", .secret = kSettingsSecret}}});
  VisionComponentHost host(fixture.engine.hostInput(fixture.models.path, fixture.fetch()));
  service.attachComponents(host);
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  const auto server = builder.BuildAndStart();
  const auto stub = wire::Settings::NewStub(server->InProcessChannel({}));
  const auto spec = visionSpec(fixture.weights);

  const auto states = [&] {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSettingsSecret);
    wire::ComponentStatesRequest request;
    component_wire::fill(*request.add_components(), spec);
    wire::ComponentStatesResponse response;
    REQUIRE(stub->ComponentStates(&context, request, &response).ok());
    REQUIRE(response.components_size() == 1);
    return component_wire::statusFrom(response.components(0));
  };
  CHECK(states().state == ComponentState::Missing);

  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSettingsSecret);
    wire::ComponentRequest request;
    component_wire::fill(*request.mutable_component(), spec);
    wire::ComponentResponse response;
    REQUIRE(stub->InstallComponent(&context, request, &response).ok());
  }
  CHECK(eventually([&] { return states().ready; }));
  CHECK(states().state == ComponentState::Installed);
  server->Shutdown();
}

TEST_CASE("the engine boots only when both model files are on disk, under the components root")
{
  TempDir models;
  const auto model = models.path / "lm.gguf";
  const auto mmproj = models.path / "mmproj.gguf";
  const ScopedConfig config("[vision]\nmodel_path = \"" + model.string() + "\"\nmmproj_path = \"" +
                            mmproj.string() + "\"\n\n[components]\nmodels_dir = \"" + models.path.string() +
                            "\"\n");
  CHECK_FALSE(resolveVisionModelFiles().present());
  std::ofstream(model) << "x";
  CHECK_FALSE(resolveVisionModelFiles().present());
  std::ofstream(mmproj) << "x";
  CHECK(resolveVisionModelFiles().present());
  CHECK(VlmConfig::resolveComponentsRoot() == models.path);
}

TEST_CASE("the components root defaults to the models directory beside the binary")
{
  const ScopedConfig config("[vision]\nmax_tokens = 64\n");
  CHECK(VlmConfig::resolveComponentsRoot() == fs::path("models"));
}

TEST_CASE("an engine that is not loaded refuses a description")
{
  VisionService service;
  CHECK_FALSE(service.isLoaded());
  cv::Mat image(8, 8, CV_8UC3, cv::Scalar(10, 20, 30));
  CHECK_THROWS_AS(static_cast<void>(service.describeMat({.bgr = image, .prompt = {}, .maxTokens = 8})),
                  ResponseException);
}
