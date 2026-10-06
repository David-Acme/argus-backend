#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/llm-config.hxx>
#include <feature/settings/llm-components.hxx>
#include <feature/settings/llm-settings.hxx>
#include <settings/component-wire.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{
namespace fs = std::filesystem;
namespace wire = argus::settings::v1;

constexpr const char* kSettingsSecret = "llm-components-test-settings";
constexpr const char* kHostCommand = "services/llm/scripts/provision.sh";

struct TempDir
{
  fs::path path;

  TempDir()
      : path(fs::temp_directory_path() /
             ("argus-llm-components-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
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
      : path_((fs::temp_directory_path() / "llm-components-test.toml").string())
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

void writeBytes(const fs::path& path, std::size_t count)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << std::string(count, 'x');
}

ComponentSpec catalogSpec()
{
  return {.id = std::string(kLlmComponent),
          .source = ComponentSource::Provisioned,
          .files = {
                    {.path = "llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf", .url = {}, .sizeBytes = 7, .sha256 = {}}},
          .hostCommand = kHostCommand};
}

struct Owner
{
  SettingsRegistry registry{llmSettingsCatalog()};
  SettingsRpcService service{{.service = "llm",
                              .registry = &registry,
                              .credentials = {{.service = "settings", .secret = kSettingsSecret}}}};
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<wire::Settings::Stub> stub;

  explicit Owner(ComponentHost& host)
  {
    service.attachComponents(host);
    grpc::ServerBuilder builder;
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    stub = wire::Settings::NewStub(server->InProcessChannel({}));
  }

  ~Owner() { server->Shutdown(); }

  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;

  using Call = grpc::Status (wire::Settings::Stub::*)(grpc::ClientContext*, const wire::ComponentRequest&,
                                                      wire::ComponentResponse*);

  [[nodiscard]] ComponentStatus states(const ComponentSpec& spec) const
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSettingsSecret);
    wire::ComponentStatesRequest request;
    component_wire::fill(*request.add_components(), spec);
    wire::ComponentStatesResponse response;
    REQUIRE(stub->ComponentStates(&context, request, &response).ok());
    REQUIRE(response.components_size() == 1);
    return component_wire::statusFrom(response.components(0));
  }

  [[nodiscard]] grpc::Status call(Call method, const ComponentSpec& spec, ComponentStatus& status) const
  {
    grpc::ClientContext context;
    context.AddMetadata(argus::client::kCallerCredentialKey, kSettingsSecret);
    wire::ComponentRequest request;
    component_wire::fill(*request.mutable_component(), spec);
    wire::ComponentResponse response;
    const auto result = (stub.get()->*method)(&context, request, &response);
    if (result.ok())
      status = component_wire::statusFrom(response.status());
    return result;
  }
};
}

TEST_CASE("the llm component is read from the files under the components root")
{
  TempDir models;
  const ScopedConfig config("[components]\nmodels_dir = \"" + models.path.string() + "\"\n");
  REQUIRE(LlmConfig::resolveComponentsRoot() == models.path);
  std::atomic<bool> loaded{false};
  DiskComponentHost host(llmComponents({.modelsDir = LlmConfig::resolveComponentsRoot(), .loaded = [&loaded] { return loaded.load(); }}));
  const Owner owner(host);
  const auto spec = catalogSpec();
  const auto states = [&] { return owner.states(spec); };

  const auto missing = states();
  CHECK(missing.state == ComponentState::HostOnly);
  CHECK(missing.bytesPresent == 0);
  CHECK(missing.bytesTotal == 7);
  CHECK(missing.hostCommand == kHostCommand);
  CHECK_FALSE(missing.ready);

  writeBytes(models.path / "llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf", 3);
  const auto installed = states();
  CHECK(installed.state == ComponentState::Installed);
  CHECK(installed.bytesPresent == 7);
  CHECK_FALSE(installed.ready);

  loaded = true;
  CHECK(states().ready);
}

TEST_CASE("installing the llm component answers host_only with the host command and fetches nothing")
{
  TempDir models;
  DiskComponentHost host(llmComponents({.modelsDir = models.path, .loaded = [] { return true; }}));
  const Owner owner(host);
  ComponentStatus status;
  REQUIRE(owner.call(&wire::Settings::Stub::InstallComponent, catalogSpec(), status).ok());
  CHECK(status.state == ComponentState::HostOnly);
  CHECK(status.hostCommand == kHostCommand);
  CHECK(fs::is_empty(models.path));
}

TEST_CASE("the llm component is core: removing it is refused and its files stay")
{
  TempDir models;
  writeBytes(models.path / "llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf", 3);
  DiskComponentHost host(llmComponents({.modelsDir = models.path, .loaded = [] { return true; }}));
  const Owner owner(host);
  ComponentStatus status;
  CHECK(owner.call(&wire::Settings::Stub::RemoveComponent, catalogSpec(), status).error_code() ==
        grpc::StatusCode::INVALID_ARGUMENT);
  const auto after = owner.states(catalogSpec());
  CHECK(after.state == ComponentState::Installed);
  CHECK(after.ready);
}

TEST_CASE("a component this owner does not hold is refused")
{
  TempDir models;
  DiskComponentHost host(llmComponents({.modelsDir = models.path, .loaded = [] { return true; }}));
  const Owner owner(host);
  auto foreign = catalogSpec();
  foreign.id = "vision";
  ComponentStatus status;
  CHECK(owner.call(&wire::Settings::Stub::CancelComponent, foreign, status).error_code() ==
        grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("the components root defaults to the models directory beside the binary")
{
  const ScopedConfig config("[components]\n");
  CHECK(LlmConfig::resolveComponentsRoot() == fs::path("models"));
}
