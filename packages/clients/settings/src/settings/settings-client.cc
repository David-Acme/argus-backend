#include "settings-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response/response-rpc.hxx>
#include <settings.grpc.pb.h>
#include <settings/component-wire.hxx>
#include <settings/settings-errors.hxx>

#include <optional>
#include <utility>

namespace
{
namespace wire = argus::settings::v1;

constexpr auto kMaxTimeout = std::chrono::seconds(120);

void check(const grpc::Status& status)
{
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
}

bool implemented(const grpc::Status& status)
{
  if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED)
    return false;
  check(status);
  return true;
}

void checkConfig(const SettingsClientConfig& config)
{
  if (config.target.empty() || config.credential.empty() || config.timeout.count() <= 0 ||
      config.timeout > kMaxTimeout)
    throw ResponseException(400, SettingsErrors::InvalidRequest);
}

std::optional<SettingType> typeOf(wire::SettingType type)
{
  switch (type) {
  case wire::SETTING_TYPE_TOGGLE: return SettingType::Toggle;
  case wire::SETTING_TYPE_INTEGER: return SettingType::Integer;
  case wire::SETTING_TYPE_DECIMAL: return SettingType::Decimal;
  case wire::SETTING_TYPE_CHOICE: return SettingType::Choice;
  case wire::SETTING_TYPE_TEXT: return SettingType::Text;
  default: return std::nullopt;
  }
}

SettingLevel levelOf(wire::SettingLevel level)
{
  return level == wire::SETTING_LEVEL_BASIC ? SettingLevel::Basic : SettingLevel::Advanced;
}

SettingApply applyOf(wire::SettingApply apply)
{
  switch (apply) {
  case wire::SETTING_APPLY_LIVE: return SettingApply::Live;
  case wire::SETTING_APPLY_NEXT_SESSION: return SettingApply::NextSession;
  default: return SettingApply::Restart;
  }
}

SettingRejectionReason reasonOf(wire::RejectionReason reason)
{
  switch (reason) {
  case wire::REJECTION_REASON_UNKNOWN_KEY: return SettingRejectionReason::Unknown;
  case wire::REJECTION_REASON_OUT_OF_RANGE: return SettingRejectionReason::OutOfRange;
  case wire::REJECTION_REASON_NOT_A_CHOICE: return SettingRejectionReason::NotAChoice;
  case wire::REJECTION_REASON_WRITE_FAILED: return SettingRejectionReason::WriteFailed;
  case wire::REJECTION_REASON_NOT_INSTALLED: return SettingRejectionReason::NotInstalled;
  default: return SettingRejectionReason::Invalid;
  }
}

std::optional<ChoiceAvailability> availabilityOf(wire::ChoiceAvailability availability)
{
  switch (availability) {
  case wire::CHOICE_AVAILABILITY_INSTALLED: return ChoiceAvailability::Installed;
  case wire::CHOICE_AVAILABILITY_INSTALLABLE: return ChoiceAvailability::Installable;
  case wire::CHOICE_AVAILABILITY_INSTALLING: return ChoiceAvailability::Installing;
  case wire::CHOICE_AVAILABILITY_HOST_ONLY: return ChoiceAvailability::HostOnly;
  case wire::CHOICE_AVAILABILITY_FAILED: return ChoiceAvailability::Failed;
  default: return std::nullopt;
  }
}

std::vector<ChoiceState> choiceStatesOf(const wire::Setting& setting)
{
  std::vector<ChoiceState> states;
  states.reserve(static_cast<std::size_t>(setting.choice_states_size()));
  for (const auto& state : setting.choice_states()) {
    const auto availability = availabilityOf(state.availability());
    if (!availability)
      continue;
    states.push_back({.choice = state.choice(),
                      .availability = *availability,
                      .sizeMb = state.size_mb(),
                      .hostCommand = state.host_command()});
  }
  return states;
}

ProfileOrigin originOf(wire::ProfileOrigin origin)
{
  switch (origin) {
  case wire::PROFILE_ORIGIN_RECOMMENDED: return ProfileOrigin::Recommended;
  case wire::PROFILE_ORIGIN_OWNER: return ProfileOrigin::Owner;
  case wire::PROFILE_ORIGIN_REVERTED: return ProfileOrigin::Reverted;
  default: return ProfileOrigin::None;
  }
}

wire::ProfileOrigin wireOrigin(ProfileOrigin origin)
{
  switch (origin) {
  case ProfileOrigin::None: return wire::PROFILE_ORIGIN_UNSPECIFIED;
  case ProfileOrigin::Recommended: return wire::PROFILE_ORIGIN_RECOMMENDED;
  case ProfileOrigin::Owner: return wire::PROFILE_ORIGIN_OWNER;
  case ProfileOrigin::Reverted: return wire::PROFILE_ORIGIN_REVERTED;
  }
  return wire::PROFILE_ORIGIN_UNSPECIFIED;
}

std::optional<ProfileMarker> markerOf(const wire::SettingsCatalog& catalog)
{
  if (!catalog.has_profile())
    return std::nullopt;
  const auto& marker = catalog.profile();
  return ProfileMarker{.id = marker.id(),
                       .origin = originOf(marker.origin()),
                       .appliedAt = marker.applied_at(),
                       .keys = {marker.keys().begin(), marker.keys().end()}};
}

SettingsCatalog catalogOf(const wire::SettingsCatalog& catalog)
{
  SettingsCatalog result{.service = catalog.service(),
                         .settings = {},
                         .configPath = catalog.config_path(),
                         .profile = markerOf(catalog),
                         .capabilities = {catalog.capabilities().begin(), catalog.capabilities().end()}};
  result.settings.reserve(static_cast<std::size_t>(catalog.settings_size()));
  for (const auto& setting : catalog.settings()) {
    const auto type = typeOf(setting.type());
    if (!type)
      continue;
    result.settings.push_back(
        {.spec = {.key = setting.key(),
                  .group = setting.group(),
                  .type = *type,
                  .level = levelOf(setting.level()),
                  .apply = applyOf(setting.apply()),
                  .range = {.min = setting.min(), .max = setting.max(), .step = setting.step()},
                  .choices = {setting.choices().begin(), setting.choices().end()},
                  .fallback = setting.fallback(),
                  .unit = setting.unit()},
         .value = setting.value(),
         .choiceStates = choiceStatesOf(setting),
         .pendingRestart = setting.pending_restart()});
  }
  return result;
}
}

struct SettingsClient::Impl
{
  SettingsClientConfig config;
  std::unique_ptr<wire::Settings::Stub> stub;

  void prepare(grpc::ClientContext& context) const
  {
    context.set_deadline(std::chrono::system_clock::now() + config.timeout);
    argus::client::addCallerCredential(context, config.credential);
  }
};

SettingsClient::SettingsClient(SettingsClientConfig config)
{
  checkConfig(config);
  auto stub = wire::Settings::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config), .stub = std::move(stub)});
}

SettingsClient::~SettingsClient() = default;

SettingsCatalog SettingsClient::list() const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::SettingsCatalog response;
  check(impl_->stub->List(&context, wire::ListSettingsRequest{}, &response));
  return catalogOf(response);
}

SettingsUpdateReply SettingsClient::update(const std::vector<SettingChange>& changes) const
{
  return update(changes, std::nullopt);
}

SettingsUpdateReply SettingsClient::update(const std::vector<SettingChange>& changes,
                                           const std::optional<ProfileMarker>& profile) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::UpdateSettingsRequest request;
  for (const auto& change : changes) {
    auto* entry = request.add_changes();
    entry->set_key(change.key);
    entry->set_value(change.value);
  }
  if (profile) {
    auto* marker = request.mutable_profile();
    marker->set_id(profile->id);
    marker->set_origin(wireOrigin(profile->origin));
    marker->set_applied_at(profile->appliedAt);
    for (const auto& key : profile->keys)
      marker->add_keys(key);
  }
  wire::UpdateSettingsResponse response;
  check(impl_->stub->Update(&context, request, &response));

  SettingsUpdateReply reply{.applied = {response.applied().begin(), response.applied().end()},
                            .rejected = {},
                            .catalog = catalogOf(response.catalog()),
                            .profileRecorded = response.profile_recorded()};
  reply.rejected.reserve(static_cast<std::size_t>(response.rejected_size()));
  for (const auto& rejection : response.rejected())
    reply.rejected.push_back({.key = rejection.key(), .reason = reasonOf(rejection.reason())});
  return reply;
}

std::optional<std::vector<ComponentStatus>> SettingsClient::componentStates(
    const std::vector<ComponentSpec>& components) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ComponentStatesRequest request;
  for (const auto& component : components)
    component_wire::fill(*request.add_components(), component);
  wire::ComponentStatesResponse response;
  if (!implemented(impl_->stub->ComponentStates(&context, request, &response)))
    return std::nullopt;
  std::vector<ComponentStatus> states;
  states.reserve(static_cast<std::size_t>(response.components_size()));
  for (const auto& status : response.components())
    states.push_back(component_wire::statusFrom(status));
  return states;
}

std::optional<ComponentStatus> SettingsClient::installComponent(const ComponentSpec& component) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ComponentRequest request;
  component_wire::fill(*request.mutable_component(), component);
  wire::ComponentResponse response;
  if (!implemented(impl_->stub->InstallComponent(&context, request, &response)))
    return std::nullopt;
  return component_wire::statusFrom(response.status());
}

std::optional<ComponentStatus> SettingsClient::cancelComponent(const ComponentSpec& component) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ComponentRequest request;
  component_wire::fill(*request.mutable_component(), component);
  wire::ComponentResponse response;
  if (!implemented(impl_->stub->CancelComponent(&context, request, &response)))
    return std::nullopt;
  return component_wire::statusFrom(response.status());
}

std::optional<ComponentStatus> SettingsClient::removeComponent(const ComponentSpec& component) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ComponentRequest request;
  component_wire::fill(*request.mutable_component(), component);
  wire::ComponentResponse response;
  if (!implemented(impl_->stub->RemoveComponent(&context, request, &response)))
    return std::nullopt;
  return component_wire::statusFrom(response.status());
}

std::optional<ModuleDataSummary> SettingsClient::moduleDataSummary(const std::string& moduleId) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ModuleDataRequest request;
  request.set_module_id(moduleId);
  wire::ModuleDataSummaryResponse response;
  if (!implemented(impl_->stub->ModuleDataSummary(&context, request, &response)))
    return std::nullopt;
  ModuleDataSummary summary{.items = {}, .bytes = response.bytes()};
  summary.items.reserve(static_cast<std::size_t>(response.items_size()));
  for (const auto& item : response.items())
    summary.items.push_back({.kind = item.kind(), .count = item.count()});
  return summary;
}

std::optional<ModuleDataPurge> SettingsClient::purgeModuleData(const std::string& moduleId) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::ModuleDataRequest request;
  request.set_module_id(moduleId);
  wire::PurgeModuleDataResponse response;
  if (!implemented(impl_->stub->PurgeModuleData(&context, request, &response)))
    return std::nullopt;
  return ModuleDataPurge{.purged = response.purged(), .reason = response.reason()};
}

std::optional<PinVerdict> SettingsClient::verifyOwnerPin(std::int64_t userId, const std::string& pin) const
{
  grpc::ClientContext context;
  impl_->prepare(context);
  wire::VerifyOwnerPinRequest request;
  request.set_user_id(userId);
  request.set_pin(pin);
  wire::VerifyOwnerPinResponse response;
  if (!implemented(impl_->stub->VerifyOwnerPin(&context, request, &response)))
    return std::nullopt;
  return component_wire::verdictFrom(response.verdict());
}

struct ModulesClient::Impl
{
  SettingsClientConfig config;
  std::unique_ptr<wire::Modules::Stub> stub;
};

ModulesClient::ModulesClient(SettingsClientConfig config)
{
  checkConfig(config);
  auto stub = wire::Modules::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config), .stub = std::move(stub)});
}

ModulesClient::~ModulesClient() = default;

ModuleStatesReply ModulesClient::moduleStates() const
{
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->config.timeout);
  argus::client::addCallerCredential(context, impl_->config.credential);
  wire::ModuleStatesResponse response;
  check(impl_->stub->ModuleStates(&context, wire::ModuleStatesRequest{}, &response));
  ModuleStatesReply reply{.modules = {}, .version = response.version(), .settled = response.settled()};
  reply.modules.reserve(static_cast<std::size_t>(response.modules_size()));
  for (const auto& module : response.modules())
    reply.modules.push_back({.id = module.id(),
                             .enabled = module.enabled(),
                             .lifecycle = module.lifecycle(),
                             .dataPurgedAt = module.data_purged_at()});
  return reply;
}
