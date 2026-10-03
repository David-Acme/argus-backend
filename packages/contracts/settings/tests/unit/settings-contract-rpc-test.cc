#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace
{
namespace wire = argus::settings::v1;

constexpr const char* kSecret = "settings-caller-secret";

struct Harness
{
  SettingsRegistry registry;
  SettingsRpcService service;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<wire::Settings::Stub> stub;

  Harness()
      : registry({{.key = "tts.speed",
                   .group = "voice",
                   .type = SettingType::Decimal,
                   .level = SettingLevel::Basic,
                   .apply = SettingApply::Live,
                   .range = {.min = 0.7, .max = 2.0, .step = 0.05},
                   .choices = {},
                   .fallback = "1"}}),
        service({.service = "tts",
                 .registry = &registry,
                 .credentials = {{.service = "settings", .secret = kSecret}}})
  {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    stub = wire::Settings::NewStub(server->InProcessChannel({}));
  }

  ~Harness() { server->Shutdown(); }
};

void authorize(grpc::ClientContext& context, const std::string& secret)
{
  context.AddMetadata(argus::client::kCallerCredentialKey, secret);
}
}

TEST_CASE("a caller without the settings credential is refused")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  Harness harness;

  grpc::ClientContext anonymous;
  wire::SettingsCatalog catalog;
  CHECK(harness.stub->List(&anonymous, {}, &catalog).error_code() == grpc::StatusCode::UNAUTHENTICATED);

  grpc::ClientContext wrong;
  authorize(wrong, "not-the-secret");
  CHECK(harness.stub->List(&wrong, {}, &catalog).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  std::remove(path.c_str());
}

TEST_CASE("list maps the catalog onto the wire")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.25\n";
  ConfigService::load(path);
  Harness harness;

  grpc::ClientContext context;
  authorize(context, kSecret);
  wire::SettingsCatalog catalog;
  REQUIRE(harness.stub->List(&context, {}, &catalog).ok());
  CHECK(catalog.service() == "tts");
  REQUIRE(catalog.settings_size() == 1);
  const auto& setting = catalog.settings(0);
  CHECK(setting.key() == "tts.speed");
  CHECK(setting.type() == wire::SETTING_TYPE_DECIMAL);
  CHECK(setting.level() == wire::SETTING_LEVEL_BASIC);
  CHECK(setting.apply() == wire::SETTING_APPLY_LIVE);
  CHECK(setting.min() == doctest::Approx(0.7));
  CHECK(setting.value() == "1.25");
  std::remove(path.c_str());
}

TEST_CASE("update applies valid changes and reports rejected ones with the fresh catalog")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  Harness harness;

  grpc::ClientContext accepted;
  authorize(accepted, kSecret);
  wire::UpdateSettingsRequest request;
  auto* change = request.add_changes();
  change->set_key("tts.speed");
  change->set_value("1.5");
  wire::UpdateSettingsResponse response;
  REQUIRE(harness.stub->Update(&accepted, request, &response).ok());
  CHECK(response.applied_size() == 1);
  CHECK(response.rejected_size() == 0);
  CHECK(response.catalog().settings(0).value() == "1.5");

  grpc::ClientContext rejected;
  authorize(rejected, kSecret);
  change->set_value("9");
  response.Clear();
  REQUIRE(harness.stub->Update(&rejected, request, &response).ok());
  REQUIRE(response.rejected_size() == 1);
  CHECK(response.rejected(0).reason() == wire::REJECTION_REASON_OUT_OF_RANGE);
  CHECK(response.catalog().settings(0).value() == "1.5");

  grpc::ClientContext empty;
  authorize(empty, kSecret);
  response.Clear();
  CHECK(harness.stub->Update(&empty, {}, &response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  std::remove(path.c_str());
}

TEST_CASE("each choice's installation state reaches the wire, and a host-only choice is refused as not installed")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\npocket_variant_es = \"fast\"\n";
  ConfigService::load(path);
  SettingsRegistry registry({{.key = "tts.pocket_variant_es",
                              .group = "engine",
                              .type = SettingType::Choice,
                              .level = SettingLevel::Basic,
                              .apply = SettingApply::Live,
                              .range = {},
                              .choices = {"fast", "quality"},
                              .fallback = "fast"}});
  registry.describeChoices([](const SettingSpec&) {
    return std::vector<ChoiceState>{
        {.choice = "fast", .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = ""},
        {.choice = "quality",
         .availability = ChoiceAvailability::HostOnly,
         .sizeMb = 672.4,
         .hostCommand = "services/tts/scripts/provision.sh --variant es-quality"}};
  });
  SettingsRpcService service(
      {.service = "tts", .registry = &registry, .credentials = {{.service = "settings", .secret = kSecret}}});
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  const auto server = builder.BuildAndStart();
  const auto stub = wire::Settings::NewStub(server->InProcessChannel({}));

  grpc::ClientContext listing;
  authorize(listing, kSecret);
  wire::SettingsCatalog catalog;
  REQUIRE(stub->List(&listing, {}, &catalog).ok());
  REQUIRE(catalog.settings_size() == 1);
  const auto& states = catalog.settings(0).choice_states();
  REQUIRE(states.size() == 2);
  CHECK(states[0].choice() == "fast");
  CHECK(states[0].availability() == wire::CHOICE_AVAILABILITY_INSTALLED);
  CHECK(states[1].availability() == wire::CHOICE_AVAILABILITY_HOST_ONLY);
  CHECK(states[1].size_mb() == doctest::Approx(672.4));
  CHECK(states[1].host_command() == "services/tts/scripts/provision.sh --variant es-quality");

  grpc::ClientContext updating;
  authorize(updating, kSecret);
  wire::UpdateSettingsRequest request;
  auto* change = request.add_changes();
  change->set_key("tts.pocket_variant_es");
  change->set_value("quality");
  wire::UpdateSettingsResponse response;
  REQUIRE(stub->Update(&updating, request, &response).ok());
  REQUIRE(response.rejected_size() == 1);
  CHECK(response.rejected(0).reason() == wire::REJECTION_REASON_NOT_INSTALLED);
  CHECK(response.catalog().settings(0).value() == "fast");
  server->Shutdown();
  std::remove(path.c_str());
}
