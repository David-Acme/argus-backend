#include "settings-rpc.hxx"

#include <config/config-service.hxx>
#include <response/response-rpc.hxx>
#include <settings/component-wire.hxx>
#include <settings/settings-errors.hxx>

#include <exception>
#include <stdexcept>
#include <utility>

namespace
{
namespace wire = argus::settings::v1;

constexpr int kMaxChanges = 64;

wire::SettingType typeOf(SettingType type)
{
  switch (type) {
  case SettingType::Toggle: return wire::SETTING_TYPE_TOGGLE;
  case SettingType::Integer: return wire::SETTING_TYPE_INTEGER;
  case SettingType::Decimal: return wire::SETTING_TYPE_DECIMAL;
  case SettingType::Choice: return wire::SETTING_TYPE_CHOICE;
  case SettingType::Text: return wire::SETTING_TYPE_TEXT;
  }
  return wire::SETTING_TYPE_UNSPECIFIED;
}

wire::SettingLevel levelOf(SettingLevel level)
{
  return level == SettingLevel::Basic ? wire::SETTING_LEVEL_BASIC : wire::SETTING_LEVEL_ADVANCED;
}

wire::SettingApply applyOf(SettingApply apply)
{
  switch (apply) {
  case SettingApply::Live: return wire::SETTING_APPLY_LIVE;
  case SettingApply::NextSession: return wire::SETTING_APPLY_NEXT_SESSION;
  case SettingApply::Restart: return wire::SETTING_APPLY_RESTART;
  }
  return wire::SETTING_APPLY_UNSPECIFIED;
}

wire::RejectionReason reasonOf(SettingRejectionReason reason)
{
  switch (reason) {
  case SettingRejectionReason::Unknown: return wire::REJECTION_REASON_UNKNOWN_KEY;
  case SettingRejectionReason::Invalid: return wire::REJECTION_REASON_INVALID;
  case SettingRejectionReason::OutOfRange: return wire::REJECTION_REASON_OUT_OF_RANGE;
  case SettingRejectionReason::NotAChoice: return wire::REJECTION_REASON_NOT_A_CHOICE;
  case SettingRejectionReason::WriteFailed: return wire::REJECTION_REASON_WRITE_FAILED;
  case SettingRejectionReason::NotInstalled: return wire::REJECTION_REASON_NOT_INSTALLED;
  }
  return wire::REJECTION_REASON_UNSPECIFIED;
}

wire::ChoiceAvailability availabilityOf(ChoiceAvailability availability)
{
  switch (availability) {
  case ChoiceAvailability::Installed: return wire::CHOICE_AVAILABILITY_INSTALLED;
  case ChoiceAvailability::Installable: return wire::CHOICE_AVAILABILITY_INSTALLABLE;
  case ChoiceAvailability::Installing: return wire::CHOICE_AVAILABILITY_INSTALLING;
  case ChoiceAvailability::HostOnly: return wire::CHOICE_AVAILABILITY_HOST_ONLY;
  case ChoiceAvailability::Failed: return wire::CHOICE_AVAILABILITY_FAILED;
  }
  return wire::CHOICE_AVAILABILITY_UNSPECIFIED;
}

wire::ProfileOrigin originOf(ProfileOrigin origin)
{
  switch (origin) {
  case ProfileOrigin::None: return wire::PROFILE_ORIGIN_UNSPECIFIED;
  case ProfileOrigin::Recommended: return wire::PROFILE_ORIGIN_RECOMMENDED;
  case ProfileOrigin::Owner: return wire::PROFILE_ORIGIN_OWNER;
  case ProfileOrigin::Reverted: return wire::PROFILE_ORIGIN_REVERTED;
  }
  return wire::PROFILE_ORIGIN_UNSPECIFIED;
}

ProfileOrigin originFrom(wire::ProfileOrigin origin)
{
  switch (origin) {
  case wire::PROFILE_ORIGIN_RECOMMENDED: return ProfileOrigin::Recommended;
  case wire::PROFILE_ORIGIN_OWNER: return ProfileOrigin::Owner;
  case wire::PROFILE_ORIGIN_REVERTED: return ProfileOrigin::Reverted;
  default: return ProfileOrigin::None;
  }
}

ProfileMarker markerFrom(const wire::ProfileMarker& marker)
{
  return {.id = marker.id(),
          .origin = originFrom(marker.origin()),
          .appliedAt = marker.applied_at(),
          .keys = {marker.keys().begin(), marker.keys().end()}};
}

grpc::ServerUnaryReactor* finish(grpc::CallbackServerContext* context, grpc::Status status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(std::move(status));
  return reactor;
}

constexpr int kMaxComponents = 32;

grpc::Status unimplemented()
{
  return {grpc::StatusCode::UNIMPLEMENTED, "This owner installs no components"};
}

grpc::Status malformed()
{
  return argus::response::toRpcStatus(ResponseException(SettingsErrors::InvalidRequest));
}

grpc::Status ownerBusy()
{
  return argus::response::toRpcStatus(ResponseException(SettingsErrors::Unavailable));
}
}

std::vector<argus::client::CallerCredential> settingsCallers(
    const std::vector<std::pair<std::string, std::string>>& callers)
{
  std::vector<std::pair<std::string, std::string>> settings;
  for (const auto& caller : callers)
    if (caller.first == kSettingsCaller)
      settings.push_back(caller);
  return argus::client::callerCredentialsFromPairs(settings);
}

void withoutSettingsCaller(std::vector<std::pair<std::string, std::string>>& callers)
{
  std::erase_if(callers, [](const auto& caller) { return caller.first == kSettingsCaller; });
}

SettingsRpcService::SettingsRpcService(SettingsRpcInput input) : input_(std::move(input))
{
  if (input_.service.empty() || input_.registry == nullptr || input_.credentials.empty())
    throw std::invalid_argument("Invalid settings RPC configuration");
}

grpc::ServerUnaryReactor* SettingsRpcService::List(grpc::CallbackServerContext* context,
                                                   const wire::ListSettingsRequest*,
                                                   wire::SettingsCatalog* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  fillCatalog(*response);
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::Update(grpc::CallbackServerContext* context,
                                                     const wire::UpdateSettingsRequest* request,
                                                     wire::UpdateSettingsResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  const bool marks = request->has_profile();
  if ((request->changes_size() == 0 && !marks) || request->changes_size() > kMaxChanges)
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::InvalidRequest)));

  std::vector<SettingChange> changes;
  changes.reserve(static_cast<std::size_t>(request->changes_size()));
  for (const auto& change : request->changes())
    changes.push_back({.key = change.key(), .value = change.value()});

  SettingsUpdateResult result;
  if (!changes.empty())
    result = input_.registry->update(changes);
  for (const auto& key : result.applied)
    response->add_applied(key);
  for (const auto& rejection : result.rejected) {
    auto* entry = response->add_rejected();
    entry->set_key(rejection.key);
    entry->set_reason(reasonOf(rejection.reason));
  }
  if (marks && result.rejected.empty())
    response->set_profile_recorded(input_.registry->recordProfile(markerFrom(request->profile())));
  fillCatalog(*response->mutable_catalog());
  return finish(context, grpc::Status::OK);
}

void SettingsRpcService::attachComponents(ComponentHost& host)
{
  components_ = &host;
}

grpc::ServerUnaryReactor* SettingsRpcService::ComponentStates(grpc::CallbackServerContext* context,
                                                              const wire::ComponentStatesRequest* request,
                                                              wire::ComponentStatesResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (components_ == nullptr)
    return finish(context, unimplemented());
  if (request->components_size() > kMaxComponents)
    return finish(context, malformed());
  try {
    for (const auto& spec : request->components())
      component_wire::fill(*response->add_components(), components_->status(component_wire::specFrom(spec)));
  }
  catch (const std::invalid_argument&) {
    response->clear_components();
    return finish(context, malformed());
  }
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::componentCall(grpc::CallbackServerContext* context,
                                                            const wire::ComponentRequest& request,
                                                            wire::ComponentResponse& response,
                                                            ComponentAction action)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (components_ == nullptr)
    return finish(context, unimplemented());
  try {
    component_wire::fill(*response.mutable_status(),
                         (components_->*action)(component_wire::specFrom(request.component())));
  }
  catch (const std::invalid_argument&) {
    return finish(context, malformed());
  }
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::InstallComponent(grpc::CallbackServerContext* context,
                                                               const wire::ComponentRequest* request,
                                                               wire::ComponentResponse* response)
{
  return componentCall(context, *request, *response, &ComponentHost::install);
}

grpc::ServerUnaryReactor* SettingsRpcService::CancelComponent(grpc::CallbackServerContext* context,
                                                              const wire::ComponentRequest* request,
                                                              wire::ComponentResponse* response)
{
  return componentCall(context, *request, *response, &ComponentHost::cancel);
}

grpc::ServerUnaryReactor* SettingsRpcService::RemoveComponent(grpc::CallbackServerContext* context,
                                                               const wire::ComponentRequest* request,
                                                               wire::ComponentResponse* response)
{
  return componentCall(context, *request, *response, &ComponentHost::remove);
}

void SettingsRpcService::attachModuleData(ModuleDataHost& host)
{
  moduleData_ = &host;
}

grpc::ServerUnaryReactor* SettingsRpcService::ModuleDataSummary(grpc::CallbackServerContext* context,
                                                                const wire::ModuleDataRequest* request,
                                                                wire::ModuleDataSummaryResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (moduleData_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner keeps no module data"});
  if (request->module_id().empty())
    return finish(context, malformed());
  ::ModuleDataSummary summary;
  try {
    summary = moduleData_->summary(request->module_id());
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  for (const auto& item : summary.items) {
    auto* entry = response->add_items();
    entry->set_kind(item.kind);
    entry->set_count(item.count);
  }
  response->set_bytes(summary.bytes);
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::PurgeModuleData(grpc::CallbackServerContext* context,
                                                              const wire::ModuleDataRequest* request,
                                                              wire::PurgeModuleDataResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (moduleData_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner keeps no module data"});
  if (request->module_id().empty())
    return finish(context, malformed());
  ModuleDataPurge outcome;
  try {
    outcome = moduleData_->purge(request->module_id());
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  response->set_purged(outcome.purged);
  response->set_reason(outcome.reason);
  return finish(context, grpc::Status::OK);
}

void SettingsRpcService::attachOwnerPin(OwnerPinHost& host)
{
  ownerPin_ = &host;
}

grpc::ServerUnaryReactor* SettingsRpcService::VerifyOwnerPin(grpc::CallbackServerContext* context,
                                                             const wire::VerifyOwnerPinRequest* request,
                                                             wire::VerifyOwnerPinResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (ownerPin_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner keeps no PIN"});
  if (request->user_id() <= 0)
    return finish(context, malformed());
  try {
    response->set_verdict(
        component_wire::verdictOf(ownerPin_->verify({.userId = request->user_id(), .pin = request->pin()})));
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  return finish(context, grpc::Status::OK);
}

void SettingsRpcService::attachModuleImpact(const ModuleImpactHost& host)
{
  moduleImpact_ = &host;
}

void SettingsRpcService::attachRoleReassign(RoleReassignHost& host)
{
  roleReassign_ = &host;
}

void SettingsRpcService::attachModuleRequest(ModuleRequestHost& host)
{
  moduleRequest_ = &host;
}

grpc::ServerUnaryReactor* SettingsRpcService::ModuleImpact(grpc::CallbackServerContext* context,
                                                          const wire::ModuleImpactRequest* request,
                                                          wire::ModuleImpactResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (moduleImpact_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner reports no module impact"});
  if (request->module_id().empty())
    return finish(context, malformed());
  try {
    component_wire::fill(*response, moduleImpact_->impact(request->module_id()));
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::ReassignRoles(grpc::CallbackServerContext* context,
                                                           const wire::ReassignRolesRequest* request,
                                                           wire::ReassignRolesResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (roleReassign_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner assigns no roles"});
  if (request->actor_user_id() <= 0 || request->reassignments().empty() || request->reassignments_size() > kMaxChanges)
    return finish(context, malformed());
  try {
    component_wire::fill(*response, roleReassign_->reassign(component_wire::batchFrom(*request)));
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  return finish(context, grpc::Status::OK);
}

grpc::ServerUnaryReactor* SettingsRpcService::RequestModule(grpc::CallbackServerContext* context,
                                                           const wire::RequestModuleRequest* request,
                                                           wire::RequestModuleResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return finish(context, argus::response::toRpcStatus(ResponseException(SettingsErrors::Unauthorized)));
  if (moduleRequest_ == nullptr)
    return finish(context, {grpc::StatusCode::UNIMPLEMENTED, "This owner takes no module requests"});
  if (request->module_id().empty() || request->user_id() <= 0 || request->day().empty())
    return finish(context, malformed());
  try {
    const auto outcome = moduleRequest_->request(component_wire::requestFrom(*request));
    response->set_notified(outcome.notified);
    response->set_duplicate(outcome.duplicate);
  }
  catch (const std::exception&) {
    return finish(context, ownerBusy());
  }
  return finish(context, grpc::Status::OK);
}

void SettingsRpcService::fillCatalog(wire::SettingsCatalog& catalog) const
{
  catalog.set_service(input_.service);
  catalog.set_config_path(ConfigService::path());
  const auto marker = input_.registry->profileMarker();
  auto* profile = catalog.mutable_profile();
  profile->set_id(marker.id);
  profile->set_origin(originOf(marker.origin));
  profile->set_applied_at(marker.appliedAt);
  for (const auto& key : marker.keys)
    profile->add_keys(key);
  for (const auto& capability : input_.registry->capabilities())
    catalog.add_capabilities(capability);
  for (const auto& entry : input_.registry->list()) {
    auto* setting = catalog.add_settings();
    setting->set_key(entry.spec.key);
    setting->set_group(entry.spec.group);
    setting->set_type(typeOf(entry.spec.type));
    setting->set_level(levelOf(entry.spec.level));
    setting->set_apply(applyOf(entry.spec.apply));
    setting->set_min(entry.spec.range.min);
    setting->set_max(entry.spec.range.max);
    setting->set_step(entry.spec.range.step);
    for (const auto& choice : entry.spec.choices)
      setting->add_choices(choice);
    setting->set_value(entry.value);
    setting->set_fallback(entry.spec.fallback);
    setting->set_unit(entry.spec.unit);
    setting->set_pending_restart(entry.pendingRestart);
    for (const auto& state : entry.choiceStates) {
      auto* choice = setting->add_choice_states();
      choice->set_choice(state.choice);
      choice->set_availability(availabilityOf(state.availability));
      choice->set_size_mb(state.sizeMb);
      choice->set_host_command(state.hostCommand);
    }
  }
}
