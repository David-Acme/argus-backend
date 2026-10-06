#include "component-wire.hxx"

namespace component_wire
{
namespace wire = argus::settings::v1;

wire::ComponentSource sourceOf(ComponentSource source)
{
  return source == ComponentSource::Download ? wire::COMPONENT_SOURCE_DOWNLOAD : wire::COMPONENT_SOURCE_PROVISIONED;
}

ComponentSource sourceFrom(wire::ComponentSource source)
{
  return source == wire::COMPONENT_SOURCE_DOWNLOAD ? ComponentSource::Download : ComponentSource::Provisioned;
}

wire::ComponentState stateOf(ComponentState state)
{
  switch (state) {
  case ComponentState::Installed: return wire::COMPONENT_STATE_INSTALLED;
  case ComponentState::Missing: return wire::COMPONENT_STATE_MISSING;
  case ComponentState::Installing: return wire::COMPONENT_STATE_INSTALLING;
  case ComponentState::Failed: return wire::COMPONENT_STATE_FAILED;
  case ComponentState::HostOnly: return wire::COMPONENT_STATE_HOST_ONLY;
  }
  return wire::COMPONENT_STATE_UNSPECIFIED;
}

ComponentState stateFrom(wire::ComponentState state)
{
  switch (state) {
  case wire::COMPONENT_STATE_INSTALLED: return ComponentState::Installed;
  case wire::COMPONENT_STATE_INSTALLING: return ComponentState::Installing;
  case wire::COMPONENT_STATE_FAILED: return ComponentState::Failed;
  case wire::COMPONENT_STATE_HOST_ONLY: return ComponentState::HostOnly;
  default: return ComponentState::Missing;
  }
}

void fill(wire::ComponentSpec& wire, const ComponentSpec& spec)
{
  wire.set_id(spec.id);
  wire.set_source(sourceOf(spec.source));
  wire.set_host_command(spec.hostCommand);
  for (const auto& file : spec.files) {
    auto* entry = wire.add_files();
    entry->set_path(file.path);
    entry->set_url(file.url);
    entry->set_size_bytes(file.sizeBytes);
    entry->set_sha256(file.sha256);
  }
}

ComponentSpec specFrom(const wire::ComponentSpec& wire)
{
  ComponentSpec spec{
      .id = wire.id(), .source = sourceFrom(wire.source()), .files = {}, .hostCommand = wire.host_command()};
  spec.files.reserve(static_cast<std::size_t>(wire.files_size()));
  for (const auto& file : wire.files())
    spec.files.push_back(
        {.path = file.path(), .url = file.url(), .sizeBytes = file.size_bytes(), .sha256 = file.sha256()});
  return spec;
}

wire::PinVerdict verdictOf(PinVerdict verdict)
{
  switch (verdict) {
  case PinVerdict::NoPin: return wire::PIN_VERDICT_NO_PIN;
  case PinVerdict::Accepted: return wire::PIN_VERDICT_ACCEPTED;
  case PinVerdict::Required: return wire::PIN_VERDICT_REQUIRED;
  case PinVerdict::Invalid: return wire::PIN_VERDICT_INVALID;
  case PinVerdict::Locked: return wire::PIN_VERDICT_LOCKED;
  }
  return wire::PIN_VERDICT_UNSPECIFIED;
}

PinVerdict verdictFrom(wire::PinVerdict verdict)
{
  switch (verdict) {
  case wire::PIN_VERDICT_NO_PIN: return PinVerdict::NoPin;
  case wire::PIN_VERDICT_ACCEPTED: return PinVerdict::Accepted;
  case wire::PIN_VERDICT_INVALID: return PinVerdict::Invalid;
  case wire::PIN_VERDICT_LOCKED: return PinVerdict::Locked;
  default: return PinVerdict::Required;
  }
}

void fill(wire::ComponentStatus& wire, const ComponentStatus& status)
{
  wire.set_id(status.id);
  wire.set_state(stateOf(status.state));
  wire.set_bytes_present(status.bytesPresent);
  wire.set_bytes_total(status.bytesTotal);
  wire.set_ready(status.ready);
  wire.set_host_command(status.hostCommand);
  wire.set_reason(status.reason);
}

ComponentStatus statusFrom(const wire::ComponentStatus& wire)
{
  return {.id = wire.id(),
          .state = stateFrom(wire.state()),
          .bytesPresent = wire.bytes_present(),
          .bytesTotal = wire.bytes_total(),
          .ready = wire.ready(),
          .hostCommand = wire.host_command(),
          .reason = wire.reason()};
}

void fill(wire::ModuleImpactResponse& wire, const ModuleImpactReport& report)
{
  for (const auto& stop : report.stops) {
    auto* entry = wire.add_stops();
    entry->set_kind(stop.kind);
    entry->set_count(stop.count);
  }
  for (const auto& holder : report.roleHolders) {
    auto* entry = wire.add_role_holders();
    entry->set_user_id(holder.userId);
    entry->set_name(holder.name);
    entry->set_last_name(holder.lastName);
    entry->set_role(holder.role);
    entry->set_is_active(holder.isActive);
  }
  for (const auto& invitation : report.invitations) {
    auto* entry = wire.add_invitations();
    entry->set_id(invitation.id);
    entry->set_role(invitation.role);
    entry->set_created_by(invitation.createdBy);
    entry->set_created_by_name(invitation.createdByName);
    entry->set_expires_at(invitation.expiresAt);
  }
}

ModuleImpactReport impactFrom(const wire::ModuleImpactResponse& wire)
{
  ModuleImpactReport report;
  for (const auto& stop : wire.stops())
    report.stops.push_back({.kind = stop.kind(), .count = stop.count()});
  for (const auto& holder : wire.role_holders())
    report.roleHolders.push_back({.userId = holder.user_id(),
                                  .name = holder.name(),
                                  .lastName = holder.last_name(),
                                  .role = holder.role(),
                                  .isActive = holder.is_active()});
  for (const auto& invitation : wire.invitations())
    report.invitations.push_back({.id = invitation.id(),
                                  .role = invitation.role(),
                                  .createdBy = invitation.created_by(),
                                  .createdByName = invitation.created_by_name(),
                                  .expiresAt = invitation.expires_at()});
  return report;
}

void fill(wire::ReassignRolesRequest& wire, const RoleReassignmentBatch& batch)
{
  wire.set_actor_user_id(batch.actorUserId);
  for (const auto& entry : batch.reassignments) {
    auto* item = wire.add_reassignments();
    item->set_user_id(entry.userId);
    item->set_role(entry.role);
  }
}

RoleReassignmentBatch batchFrom(const wire::ReassignRolesRequest& wire)
{
  RoleReassignmentBatch batch{.actorUserId = wire.actor_user_id(), .reassignments = {}};
  for (const auto& item : wire.reassignments())
    batch.reassignments.push_back({.userId = item.user_id(), .role = item.role()});
  return batch;
}

void fill(wire::ReassignRolesResponse& wire, const RoleReassignmentOutcome& outcome)
{
  switch (outcome.status) {
  case ReassignStatus::Applied: wire.set_status(wire::REASSIGN_STATUS_APPLIED); break;
  case ReassignStatus::Refused: wire.set_status(wire::REASSIGN_STATUS_REFUSED); break;
  case ReassignStatus::Failed: wire.set_status(wire::REASSIGN_STATUS_FAILED); break;
  }
  wire.set_applied(outcome.applied);
  wire.set_failed_user_id(outcome.failedUserId);
  wire.set_reason(outcome.reason);
}

RoleReassignmentOutcome outcomeFrom(const wire::ReassignRolesResponse& wire)
{
  RoleReassignmentOutcome outcome{
      .status = ReassignStatus::Failed,
      .applied = wire.applied(),
      .failedUserId = wire.failed_user_id(),
      .reason = wire.reason()};
  if (wire.status() == wire::REASSIGN_STATUS_APPLIED)
    outcome.status = ReassignStatus::Applied;
  else if (wire.status() == wire::REASSIGN_STATUS_REFUSED)
    outcome.status = ReassignStatus::Refused;
  return outcome;
}

void fill(wire::RequestModuleRequest& wire, const ModuleRequestInput& input)
{
  wire.set_module_id(input.moduleId);
  wire.set_module_name_es(input.moduleName.es);
  wire.set_module_name_en(input.moduleName.en);
  wire.set_user_id(input.userId);
  wire.set_day(input.day);
}

ModuleRequestInput requestFrom(const wire::RequestModuleRequest& wire)
{
  return {.moduleId = wire.module_id(),
          .moduleName = {.es = wire.module_name_es(), .en = wire.module_name_en()},
          .userId = wire.user_id(),
          .day = wire.day()};
}
}
