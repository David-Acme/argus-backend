#pragma once

#include <settings.pb.h>
#include <settings/component-vocabulary.hxx>

namespace component_wire
{
[[nodiscard]] argus::settings::v1::ComponentSource sourceOf(ComponentSource source);
[[nodiscard]] ComponentSource sourceFrom(argus::settings::v1::ComponentSource source);
[[nodiscard]] argus::settings::v1::ComponentState stateOf(ComponentState state);
[[nodiscard]] ComponentState stateFrom(argus::settings::v1::ComponentState state);

void fill(argus::settings::v1::ComponentSpec& wire, const ComponentSpec& spec);
[[nodiscard]] ComponentSpec specFrom(const argus::settings::v1::ComponentSpec& wire);

void fill(argus::settings::v1::ComponentStatus& wire, const ComponentStatus& status);
[[nodiscard]] argus::settings::v1::PinVerdict verdictOf(PinVerdict verdict);
[[nodiscard]] PinVerdict verdictFrom(argus::settings::v1::PinVerdict verdict);
[[nodiscard]] ComponentStatus statusFrom(const argus::settings::v1::ComponentStatus& wire);

void fill(argus::settings::v1::ModuleImpactResponse& wire, const ModuleImpactReport& report);
[[nodiscard]] ModuleImpactReport impactFrom(const argus::settings::v1::ModuleImpactResponse& wire);

void fill(argus::settings::v1::ReassignRolesRequest& wire, const RoleReassignmentBatch& batch);
[[nodiscard]] RoleReassignmentBatch batchFrom(const argus::settings::v1::ReassignRolesRequest& wire);
void fill(argus::settings::v1::ReassignRolesResponse& wire, const RoleReassignmentOutcome& outcome);
[[nodiscard]] RoleReassignmentOutcome outcomeFrom(const argus::settings::v1::ReassignRolesResponse& wire);

void fill(argus::settings::v1::RequestModuleRequest& wire, const ModuleRequestInput& input);
[[nodiscard]] ModuleRequestInput requestFrom(const argus::settings::v1::RequestModuleRequest& wire);
}
