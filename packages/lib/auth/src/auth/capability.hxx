#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace role_access
{

struct CapabilitySpec
{
  std::string_view id;
  std::string_view module;
  RoleMask roles;
};

inline constexpr RoleMask kOwnerGuard = roleBits({UserRole::Owner, UserRole::Guard});
inline constexpr RoleMask kOwnerResident = roleBits({UserRole::Owner, UserRole::Resident});
inline constexpr RoleMask kOwnerResidentGuard =
    roleBits({UserRole::Owner, UserRole::Resident, UserRole::Guard});

inline constexpr std::array kCapabilities = std::to_array<CapabilitySpec>({
    {.id = "profile.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "sessions.manage", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "privacy.own", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "notifications.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "notifications.register", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "calls.join", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "heartbeat.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "modules.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "modules.request", .module = kCoreModule, .roles = kNonOwnerBaseline},
    {.id = "safety.panic", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "safety.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "safety.respond", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "reminders.read", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "reminders.write", .module = kCoreModule, .roles = kEveryRoleBaseline},
    {.id = "assistant.voice", .module = kCoreModule, .roles = kEveryRole},
    {.id = "directory.read", .module = kCoreModule, .roles = kOwnerGuard},
    {.id = "people.read", .module = kCoreModule, .roles = kOwnerResidentGuard},
    {.id = "people.write", .module = kCoreModule, .roles = kOwnerResident},
    {.id = "memory.manage", .module = kCoreModule, .roles = kOwnerResident},
    {.id = "users.manage", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "invitations.manage", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "privacy.household", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "settings.manage", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "modules.manage", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "activity.read", .module = kCoreModule, .roles = kOwnerOnly},
    {.id = "camera.view", .module = kSurveillanceModule, .roles = kEveryRole},
    {.id = "camera.talk", .module = kSurveillanceModule, .roles = kOwnerResidentGuard},
    {.id = "camera.manage", .module = kSurveillanceModule, .roles = kOwnerResident},
    {.id = "zones.read", .module = kSurveillanceModule, .roles = kOwnerResidentGuard},
    {.id = "zones.write", .module = kSurveillanceModule, .roles = kOwnerResident},
    {.id = "events.read", .module = kSurveillanceModule, .roles = kOwnerResidentGuard},
    {.id = "guard.read", .module = kSurveillanceModule, .roles = kOwnerResidentGuard},
    {.id = "guard.mode.set", .module = kSurveillanceModule, .roles = kOwnerResident},
    {.id = "guard.guests.write", .module = kSurveillanceModule, .roles = kOwnerResident},
    {.id = "guard.admin", .module = kSurveillanceModule, .roles = kOwnerOnly},
    {.id = "response.duty", .module = kSurveillanceModule, .roles = kOwnerGuard},
    {.id = "safety.duress", .module = kSurveillanceModule, .roles = kOwnerResident},
    {.id = "visitors.read", .module = kSurveillanceModule, .roles = kOwnerGuard},
    {.id = "visitors.manage", .module = kSurveillanceModule, .roles = kOwnerOnly},
    {.id = "presence.read", .module = kSurveillanceModule, .roles = kOwnerOnly},
    {.id = "agenda.read", .module = kProductivityModule, .roles = kOwnerResident},
    {.id = "agenda.write", .module = kProductivityModule, .roles = kOwnerResident},
    {.id = "projects.read", .module = kProductivityModule, .roles = kOwnerResident},
    {.id = "projects.write", .module = kProductivityModule, .roles = kOwnerResident},
});

struct CapabilitiesInput
{
  UserRole role;
  const ModuleSnapshot& modules;
};

struct HasCapabilityInput
{
  UserRole role;
  const ModuleSnapshot& modules;
  std::string_view capability;
};

inline bool capabilityGranted(const CapabilitySpec& spec, const CapabilitiesInput& input)
{
  return roleGranted(spec.roles, input.role, input.modules.roleActive(input.role)) &&
         input.modules.enabled(spec.module);
}

inline std::vector<std::string_view> capabilitiesFor(const CapabilitiesInput& input)
{
  std::vector<std::string_view> granted;
  for (const auto& spec : kCapabilities) {
    if (capabilityGranted(spec, input))
      granted.push_back(spec.id);
  }
  return granted;
}

inline bool hasCapability(const HasCapabilityInput& input)
{
  const auto spec = std::ranges::find(kCapabilities, input.capability, &CapabilitySpec::id);
  return spec != kCapabilities.end() &&
         capabilityGranted(*spec, {.role = input.role, .modules = input.modules});
}

struct AppActionInput
{
  UserRole role;
  AppAction action;
  const ModuleSnapshot& modules;
};

inline bool hasAppAction(const AppActionInput& input)
{
  const std::string_view capability = [&] {
    switch (input.action) {
      case AppAction::ShowCamera:
        return std::string_view("camera.view");
      case AppAction::OpenScreen:
        return std::string_view("notifications.read");
      case AppAction::SetGuardMode:
        return std::string_view("guard.mode.set");
    }
    return std::string_view();
  }();
  return hasCapability({.role = input.role, .modules = input.modules, .capability = capability});
}

inline bool knownCapability(std::string_view capability)
{
  return std::ranges::find(kCapabilities, capability, &CapabilitySpec::id) != kCapabilities.end();
}

}
