#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <settings/component-host.hxx>
#include <settings/component-wire.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
namespace wire = argus::settings::v1;
namespace fs = std::filesystem;

constexpr const char* kSecret = "component-host-secret";
constexpr const char* kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct TempDir
{
  fs::path path;

  TempDir()
      : path(fs::temp_directory_path() /
             ("argus-component-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
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

void writeBytes(const fs::path& path, std::size_t count)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << std::string(count, 'x');
}

ComponentSpec downloadSpec()
{
  return {.id = "vision",
          .source = ComponentSource::Download,
          .files = {{.path = "vision/lm.gguf", .url = "https://example.test/lm.gguf", .sizeBytes = 10, .sha256 = kSha},
                    {.path = "vision/mmproj.gguf",
                     .url = "https://example.test/mmproj.gguf",
                     .sizeBytes = 6,
                     .sha256 = kSha}},
          .hostCommand = {}};
}

ComponentSpec provisionedSpec()
{
  return {.id = "detector",
          .source = ComponentSource::Provisioned,
          .files = {{.path = "objects/yolo26n.param", .url = {}, .sizeBytes = 4, .sha256 = {}}},
          .hostCommand = "services/camera/scripts/provision.sh"};
}

template <typename Predicate>
bool eventually(const Predicate& predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::yield();
  }
  return predicate();
}

std::string writeTarget(const ComponentFetchInput& input)
{
  std::string reason;
  writeBytes(input.target, static_cast<std::size_t>(input.file.sizeBytes));
  if (!fs::exists(input.target))
    reason = input.file.path;
  return reason;
}
}

TEST_CASE("a component spec must be relative, sized and pinned when downloaded")
{
  CHECK(DiskComponentHost::acceptable(downloadSpec()));
  CHECK(DiskComponentHost::acceptable(provisionedSpec()));

  auto escape = downloadSpec();
  escape.files[0].path = "../etc/passwd";
  CHECK_FALSE(DiskComponentHost::acceptable(escape));

  auto absolute = downloadSpec();
  absolute.files[0].path = "/etc/passwd";
  CHECK_FALSE(DiskComponentHost::acceptable(absolute));

  auto unpinned = downloadSpec();
  unpinned.files[1].sha256.clear();
  CHECK_FALSE(DiskComponentHost::acceptable(unpinned));

  auto plain = downloadSpec();
  plain.files[0].url = "http://example.test/lm.gguf";
  CHECK_FALSE(DiskComponentHost::acceptable(plain));

  auto empty = downloadSpec();
  empty.files.clear();
  CHECK_FALSE(DiskComponentHost::acceptable(empty));
}

TEST_CASE("the state comes from the files on disk")
{
  TempDir models;
  std::atomic<bool> loaded{false};
  DiskComponentHost host({.modelsDir = models.path,
                          .owned = {"vision", "detector"},
                          .fetch = writeTarget,
                          .ready = [&loaded](const std::string&) { return loaded.load(); }});

  const auto provisioned = host.status(provisionedSpec());
  CHECK(provisioned.state == ComponentState::HostOnly);
  CHECK(provisioned.hostCommand == "services/camera/scripts/provision.sh");
  CHECK(provisioned.bytesTotal == 4);

  auto missing = host.status(downloadSpec());
  CHECK(missing.state == ComponentState::Missing);
  CHECK(missing.bytesTotal == 16);
  CHECK(missing.bytesPresent == 0);
  CHECK(missing.hostCommand.empty());

  writeBytes(models.path / "vision/lm.gguf.part", 4);
  writeBytes(models.path / "vision/mmproj.gguf", 6);
  const auto partial = host.status(downloadSpec());
  CHECK(partial.state == ComponentState::Missing);
  CHECK(partial.bytesPresent == 10);

  writeBytes(models.path / "vision/lm.gguf", 10);
  auto installed = host.status(downloadSpec());
  CHECK(installed.state == ComponentState::Installed);
  CHECK(installed.bytesPresent == 16);
  CHECK_FALSE(installed.ready);
  loaded = true;
  CHECK(host.status(downloadSpec()).ready);

  writeBytes(models.path / "objects/yolo26n.param", 4);
  CHECK(host.status(provisionedSpec()).state == ComponentState::Installed);
}

TEST_CASE("installing a download fetches every missing file and a provisioned one stays host-only")
{
  TempDir models;
  std::atomic<int> fetched{0};
  DiskComponentHost host({.modelsDir = models.path,
                          .owned = {"vision", "detector"},
                          .fetch =
                              [&fetched](const ComponentFetchInput& input) {
                                ++fetched;
                                return writeTarget(input);
                              },
                          .ready = {}});

  writeBytes(models.path / "vision/mmproj.gguf", 6);
  host.install(downloadSpec());
  CHECK(eventually([&] { return host.status(downloadSpec()).state == ComponentState::Installed; }));
  CHECK(fetched.load() == 1);
  CHECK(host.status(downloadSpec()).ready);

  CHECK(host.install(provisionedSpec()).state == ComponentState::HostOnly);
}

TEST_CASE("a failed fetch is reported with its reason and a retry starts over")
{
  TempDir models;
  std::atomic<bool> fail{true};
  DiskComponentHost host({.modelsDir = models.path,
                          .owned = {"vision"},
                          .fetch =
                              [&fail](const ComponentFetchInput& input) {
                                return fail.load() ? std::string("checksum_mismatch") : writeTarget(input);
                              },
                          .ready = {}});

  host.install(downloadSpec());
  CHECK(eventually([&] { return host.status(downloadSpec()).state == ComponentState::Failed; }));
  CHECK(host.status(downloadSpec()).reason == "checksum_mismatch");

  fail = false;
  host.install(downloadSpec());
  CHECK(eventually([&] { return host.status(downloadSpec()).state == ComponentState::Installed; }));
}

TEST_CASE("cancel stops a running fetch and leaves the component missing, remove deletes its files")
{
  TempDir models;
  std::mutex mutex;
  std::condition_variable entered;
  bool inside = false;
  DiskComponentHost host({.modelsDir = models.path,
                          .owned = {"vision"},
                          .fetch =
                              [&](const ComponentFetchInput& input) {
                                writeBytes(fs::path(input.target.string() + ".part"), 3);
                                {
                                  const std::scoped_lock lock(mutex);
                                  inside = true;
                                }
                                entered.notify_all();
                                std::mutex wait;
                                std::condition_variable_any stopped;
                                std::unique_lock lock(wait);
                                stopped.wait(lock, input.stop, [] { return false; });
                                return std::string("network");
                              },
                          .ready = {}});

  CHECK(host.install(downloadSpec()).state == ComponentState::Installing);
  {
    std::unique_lock lock(mutex);
    CHECK(entered.wait_for(lock, std::chrono::seconds(10), [&] { return inside; }));
  }
  const auto cancelled = host.cancel(downloadSpec());
  CHECK(cancelled.state == ComponentState::Missing);
  CHECK(cancelled.bytesPresent == 3);
  CHECK(cancelled.reason.empty());

  const auto removed = host.remove(downloadSpec());
  CHECK(removed.bytesPresent == 0);
  CHECK_FALSE(fs::exists(models.path / "vision/lm.gguf.part"));
}

TEST_CASE("a component that is not this owner's is refused")
{
  TempDir models;
  DiskComponentHost host({.modelsDir = models.path, .owned = {"detector"}, .fetch = writeTarget, .ready = {}});
  CHECK_THROWS_AS(static_cast<void>(host.status(downloadSpec())), std::invalid_argument);
}

TEST_CASE("the component RPCs answer UNIMPLEMENTED without a host and delegate with one")
{
  const std::string path = "/tmp/component-host-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  SettingsRegistry registry({{.key = "tts.speed",
                              .group = "voice",
                              .type = SettingType::Decimal,
                              .level = SettingLevel::Basic,
                              .apply = SettingApply::Live,
                              .range = {.min = 0.7, .max = 2.0, .step = 0.05},
                              .choices = {},
                              .fallback = "1"}});
  SettingsRpcService service({.service = "camera",
                              .registry = &registry,
                              .credentials = {{.service = "settings", .secret = kSecret}}});
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  auto stub = wire::Settings::NewStub(server->InProcessChannel({}));

  wire::ComponentStatesRequest request;
  component_wire::fill(*request.add_components(), provisionedSpec());
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSecret);
    wire::ComponentStatesResponse response;
    CHECK(stub->ComponentStates(&context, request, &response).error_code() == grpc::StatusCode::UNIMPLEMENTED);
  }

  TempDir models;
  DiskComponentHost host({.modelsDir = models.path, .owned = {"detector"}, .fetch = {}, .ready = {}});
  service.attachComponents(host);
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSecret);
    wire::ComponentStatesResponse response;
    REQUIRE(stub->ComponentStates(&context, request, &response).ok());
    REQUIRE(response.components_size() == 1);
    const auto status = component_wire::statusFrom(response.components(0));
    CHECK(status.state == ComponentState::HostOnly);
    CHECK(status.hostCommand == "services/camera/scripts/provision.sh");
  }
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSecret);
    wire::ComponentRequest install;
    component_wire::fill(*install.mutable_component(), downloadSpec());
    wire::ComponentResponse response;
    CHECK(stub->InstallComponent(&context, install, &response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, "wrong");
    wire::ComponentStatesResponse response;
    CHECK(stub->ComponentStates(&context, request, &response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }
  server->Shutdown();
}
