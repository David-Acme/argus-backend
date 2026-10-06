#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <settings/component-wire.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
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

namespace
{
class FailingModuleData final : public ModuleDataHost
{
public:
  [[nodiscard]] ModuleDataSummary summary(const std::string&) const override
  {
    throw std::runtime_error("the owner's database is not open");
  }

  ModuleDataPurge purge(const std::string&) override { throw std::runtime_error("the owner's database is not open"); }
};

class FailingOwnerPin final : public OwnerPinHost
{
public:
  PinVerdict verify(const OwnerPinCheck&) override { throw std::runtime_error("the owner's database is not open"); }
};
}

TEST_CASE("an owner whose data or PIN host fails answers UNAVAILABLE instead of ending the process")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  Harness harness;
  FailingModuleData data;
  FailingOwnerPin pin;
  harness.service.attachModuleData(data);
  harness.service.attachOwnerPin(pin);

  wire::ModuleDataRequest request;
  request.set_module_id("surveillance");

  grpc::ClientContext summaryContext;
  authorize(summaryContext, kSecret);
  wire::ModuleDataSummaryResponse summary;
  CHECK(harness.stub->ModuleDataSummary(&summaryContext, request, &summary).error_code() ==
        grpc::StatusCode::UNAVAILABLE);

  grpc::ClientContext purgeContext;
  authorize(purgeContext, kSecret);
  wire::PurgeModuleDataResponse purged;
  CHECK(harness.stub->PurgeModuleData(&purgeContext, request, &purged).error_code() == grpc::StatusCode::UNAVAILABLE);

  wire::VerifyOwnerPinRequest pinRequest;
  pinRequest.set_user_id(1);
  pinRequest.set_pin("2468");
  grpc::ClientContext pinContext;
  authorize(pinContext, kSecret);
  wire::VerifyOwnerPinResponse verdict;
  CHECK(harness.stub->VerifyOwnerPin(&pinContext, pinRequest, &verdict).error_code() == grpc::StatusCode::UNAVAILABLE);
  std::remove(path.c_str());
}

namespace
{
class FixedImpact final : public ModuleImpactHost
{
public:
  [[nodiscard]] ModuleImpactReport impact(const std::string& moduleId) const override
  {
    if (moduleId == "broken")
      throw std::runtime_error("the owner's database is not open");
    return {.stops = {{.kind = "pending_alerts", .count = 3}},
            .roleHolders = {{.userId = 7, .name = "Gus", .lastName = "Vela", .role = "guard", .isActive = true}},
            .invitations = {{.id = 9, .role = "guard", .createdBy = 1, .createdByName = "Olga", .expiresAt = 1700}}};
  }
};

class RecordingReassign final : public RoleReassignHost
{
public:
  RoleReassignmentOutcome reassign(const RoleReassignmentBatch& batch) override
  {
    seen = batch;
    if (batch.reassignments.size() > 1)
      return {.status = ReassignStatus::Refused, .applied = 1, .failedUserId = batch.reassignments[1].userId, .reason = "owner_required"};
    return {.status = ReassignStatus::Applied, .applied = 1, .failedUserId = 0, .reason = {}};
  }

  RoleReassignmentBatch seen;
};

class RecordingRequest final : public ModuleRequestHost
{
public:
  ModuleRequestOutcome request(const ModuleRequestInput& input) override
  {
    seen = input;
    return {.notified = 2, .duplicate = input.day == "2026-10-06-again"};
  }

  ModuleRequestInput seen;
};
}

TEST_CASE("an owner without the impact, reassign and request hosts answers UNIMPLEMENTED")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  Harness harness;

  wire::ModuleImpactRequest impactRequest;
  impactRequest.set_module_id("surveillance");
  grpc::ClientContext impactContext;
  authorize(impactContext, kSecret);
  wire::ModuleImpactResponse impact;
  CHECK(harness.stub->ModuleImpact(&impactContext, impactRequest, &impact).error_code() ==
        grpc::StatusCode::UNIMPLEMENTED);

  wire::ReassignRolesRequest reassignRequest;
  reassignRequest.set_actor_user_id(1);
  reassignRequest.add_reassignments()->set_user_id(7);
  grpc::ClientContext reassignContext;
  authorize(reassignContext, kSecret);
  wire::ReassignRolesResponse reassigned;
  CHECK(harness.stub->ReassignRoles(&reassignContext, reassignRequest, &reassigned).error_code() ==
        grpc::StatusCode::UNIMPLEMENTED);

  wire::RequestModuleRequest moduleRequest;
  moduleRequest.set_module_id("surveillance");
  moduleRequest.set_user_id(7);
  moduleRequest.set_day("2026-10-06");
  grpc::ClientContext requestContext;
  authorize(requestContext, kSecret);
  wire::RequestModuleResponse requested;
  CHECK(harness.stub->RequestModule(&requestContext, moduleRequest, &requested).error_code() ==
        grpc::StatusCode::UNIMPLEMENTED);

  grpc::ClientContext anonymous;
  CHECK(harness.stub->ModuleImpact(&anonymous, impactRequest, &impact).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  std::remove(path.c_str());
}

TEST_CASE("the impact, reassign and request answers cross the wire intact, and a failing host answers UNAVAILABLE")
{
  const std::string path = "/tmp/settings-contract-rpc.toml";
  std::ofstream(path) << "[tts]\nspeed = 1.0\n";
  ConfigService::load(path);
  Harness harness;
  const FixedImpact impactHost;
  RecordingReassign reassignHost;
  RecordingRequest requestHost;
  harness.service.attachModuleImpact(impactHost);
  harness.service.attachRoleReassign(reassignHost);
  harness.service.attachModuleRequest(requestHost);

  wire::ModuleImpactRequest impactRequest;
  impactRequest.set_module_id("surveillance");
  grpc::ClientContext impactContext;
  authorize(impactContext, kSecret);
  wire::ModuleImpactResponse impact;
  REQUIRE(harness.stub->ModuleImpact(&impactContext, impactRequest, &impact).ok());
  const auto report = component_wire::impactFrom(impact);
  REQUIRE(report.stops.size() == 1);
  CHECK(report.stops[0].kind == "pending_alerts");
  CHECK(report.stops[0].count == 3);
  REQUIRE(report.roleHolders.size() == 1);
  CHECK(report.roleHolders[0].userId == 7);
  CHECK(report.roleHolders[0].role == "guard");
  CHECK(report.roleHolders[0].lastName == "Vela");
  REQUIRE(report.invitations.size() == 1);
  CHECK(report.invitations[0].createdByName == "Olga");
  CHECK(report.invitations[0].expiresAt == 1700);

  impactRequest.set_module_id("broken");
  grpc::ClientContext brokenContext;
  authorize(brokenContext, kSecret);
  CHECK(harness.stub->ModuleImpact(&brokenContext, impactRequest, &impact).error_code() ==
        grpc::StatusCode::UNAVAILABLE);

  wire::ReassignRolesRequest reassignRequest;
  component_wire::fill(reassignRequest, {.actorUserId = 1, .reassignments = {{.userId = 7, .role = "resident"}}});
  grpc::ClientContext reassignContext;
  authorize(reassignContext, kSecret);
  wire::ReassignRolesResponse reassigned;
  REQUIRE(harness.stub->ReassignRoles(&reassignContext, reassignRequest, &reassigned).ok());
  CHECK(component_wire::outcomeFrom(reassigned).status == ReassignStatus::Applied);
  CHECK(reassignHost.seen.actorUserId == 1);
  REQUIRE(reassignHost.seen.reassignments.size() == 1);
  CHECK(reassignHost.seen.reassignments[0].role == "resident");

  wire::ReassignRolesRequest pair;
  component_wire::fill(pair, {.actorUserId = 1,
                              .reassignments = {{.userId = 7, .role = "resident"}, {.userId = 8, .role = "guest"}}});
  grpc::ClientContext refusedContext;
  authorize(refusedContext, kSecret);
  REQUIRE(harness.stub->ReassignRoles(&refusedContext, pair, &reassigned).ok());
  const auto refused = component_wire::outcomeFrom(reassigned);
  CHECK(refused.status == ReassignStatus::Refused);
  CHECK(refused.failedUserId == 8);
  CHECK(refused.reason == "owner_required");

  wire::ReassignRolesRequest empty;
  empty.set_actor_user_id(1);
  grpc::ClientContext emptyContext;
  authorize(emptyContext, kSecret);
  CHECK(harness.stub->ReassignRoles(&emptyContext, empty, &reassigned).error_code() != grpc::StatusCode::OK);

  wire::RequestModuleRequest moduleRequest;
  component_wire::fill(moduleRequest, {.moduleId = "surveillance",
                                        .moduleName = {.es = "Vigilancia", .en = "Surveillance"},
                                        .userId = 7,
                                        .day = "2026-10-06-again"});
  grpc::ClientContext requestContext;
  authorize(requestContext, kSecret);
  wire::RequestModuleResponse requested;
  REQUIRE(harness.stub->RequestModule(&requestContext, moduleRequest, &requested).ok());
  CHECK(requested.notified() == 2);
  CHECK(requested.duplicate());
  CHECK(requestHost.seen.moduleName.en == "Surveillance");
  CHECK(requestHost.seen.userId == 7);

  moduleRequest.set_user_id(0);
  grpc::ClientContext invalidContext;
  authorize(invalidContext, kSecret);
  CHECK(harness.stub->RequestModule(&invalidContext, moduleRequest, &requested).error_code() !=
        grpc::StatusCode::OK);
  std::remove(path.c_str());
}
