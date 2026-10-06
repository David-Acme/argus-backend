#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <drogon/HttpTypes.h>
#include <optional>
#include <sync/role-permission.hxx>
#include <sync/table-name.hxx>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>
#include <vector>

namespace role_access
{

using RoleMask = std::uint16_t;

inline constexpr unsigned kRoleBitCount = 15;
inline constexpr RoleMask kBaselineBit = static_cast<RoleMask>(1U << kRoleBitCount);

using PermSet = std::unordered_set<RolePermission>;
using TableAccess = std::unordered_map<TableName, PermSet>;

inline const PermSet kRead{RolePermission::Read};
inline const PermSet kReadUpdate{RolePermission::Read, RolePermission::Update};
inline const PermSet kCreateOwn{RolePermission::Create};
inline const PermSet kFull{RolePermission::Read, RolePermission::Create,
                           RolePermission::Update, RolePermission::Delete};

inline const std::unordered_map<UserRole, TableAccess> kTableAccess = {
    {UserRole::Resident,
     {{TableName::Camera, kFull},
      {TableName::CameraStream, kFull},
      {TableName::Zone, kFull},
      {TableName::Reminder, kFull},
      {TableName::ReminderDetail, kFull},
      {TableName::CalendarEvent, kFull},
      {TableName::CalendarEventShare, kFull},
      {TableName::Project, kFull},
      {TableName::ProjectMember, kFull},
      {TableName::ProjectTask, kFull},
      {TableName::Event, kFull},
      {TableName::Person, kFull},
      {TableName::ContextNote, kFull},
      {TableName::User, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn},
      {TableName::Memory, kFull}}},
    {UserRole::Guard,
     {{TableName::Camera, kRead},
      {TableName::Reminder, kFull},
      {TableName::ReminderDetail, kFull},
      {TableName::CameraStream, kRead},
      {TableName::Event, kRead},
      {TableName::Person, kRead},
      {TableName::Zone, kRead},
      {TableName::User, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
    {UserRole::Guest,
     {{TableName::Camera, kRead},
      {TableName::Reminder, kFull},
      {TableName::ReminderDetail, kFull},
      {TableName::User, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
};

struct GuardRouteAccess
{
  std::string_view path;
  drogon::HttpMethod method;
  RoleMask roles;
};

constexpr RoleMask roleBit(UserRole role)
{
  const auto index = static_cast<unsigned>(role);
  return index < kRoleBitCount ? static_cast<RoleMask>(1U << index) : RoleMask{0};
}

constexpr RoleMask roleBits(std::initializer_list<UserRole> roles)
{
  RoleMask bits = 0U;
  for (const UserRole role : roles)
    bits = static_cast<RoleMask>(bits | roleBit(role));
  return bits;
}

inline constexpr RoleMask kResidentAndGuard = roleBits({UserRole::Resident, UserRole::Guard});

inline constexpr RoleMask kResidentGuardGuest =
    roleBits({UserRole::Resident, UserRole::Guard, UserRole::Guest});

inline constexpr RoleMask kResidentGuardGuestBaseline =
    static_cast<RoleMask>(kResidentGuardGuest | kBaselineBit);

inline constexpr std::array<GuardRouteAccess, 13> kGuardAccess = {{
    {.path = "/guard/environments", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/episodes", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/mode", .method = drogon::Post, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/incidents", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/expected-guests", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/expected-guests", .method = drogon::Post, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/expected-guests", .method = drogon::Delete, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/environments/{id}/response", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/environments/{id}/duty", .method = drogon::Post, .roles = roleBit(UserRole::Guard)},
    {.path = "/guard/panic", .method = drogon::Post, .roles = kResidentGuardGuestBaseline},
    {.path = "/guard/safety", .method = drogon::Get, .roles = kResidentGuardGuestBaseline},
    {.path = "/guard/safety/pin", .method = drogon::Put, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/safety/pin", .method = drogon::Delete, .roles = roleBit(UserRole::Resident)},
}};

enum class CameraAction : std::uint8_t
{
  Talk = 0,
  Watch
};

struct CameraActionAccess
{
  CameraAction action;
  std::string_view segment;
  drogon::HttpMethod method;
  RoleMask roles;
};

inline constexpr std::array<CameraActionAccess, 2> kCameraActionAccess = {{
    {.action = CameraAction::Talk, .segment = "talk", .method = drogon::Post, .roles = kResidentAndGuard},
    {.action = CameraAction::Watch, .segment = "webrtc", .method = drogon::Post, .roles = kResidentGuardGuest},
}};

inline bool hasCameraAction(UserRole role, CameraAction action)
{
  if (role == UserRole::Owner)
    return true;
  const auto entry = std::ranges::find(kCameraActionAccess, action, &CameraActionAccess::action);
  return entry != kCameraActionAccess.end() && (entry->roles & roleBit(role)) != 0;
}

inline const CameraActionAccess* cameraActionRouteOf(std::string_view path,
                                                     drogon::HttpMethod method)
{
  constexpr std::string_view kPrefix = "/camera/";
  if (!path.starts_with(kPrefix))
    return nullptr;
  const std::string_view rest = path.substr(kPrefix.size());
  const size_t slash = rest.find('/');
  if (slash == 0 || slash == std::string_view::npos)
    return nullptr;
  const std::string_view id = rest.substr(0, slash);
  if (!std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; }))
    return nullptr;
  const std::string_view segment = rest.substr(slash + 1);
  const auto entry = std::ranges::find_if(kCameraActionAccess, [&](const CameraActionAccess& candidate) {
    return candidate.segment == segment && candidate.method == method;
  });
  return entry == kCameraActionAccess.end() ? nullptr : &*entry;
}

struct AuthRouteAccess
{
  std::string_view path;
  drogon::HttpMethod method;
  RoleMask roles;
};

inline constexpr RoleMask kEveryRole =
    roleBits({UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest});

inline constexpr RoleMask kEveryRoleBaseline = static_cast<RoleMask>(kEveryRole | kBaselineBit);

inline constexpr RoleMask kNonOwnerBaseline =
    static_cast<RoleMask>(kResidentGuardGuest | kBaselineBit);

inline constexpr RoleMask kOwnerOnly = roleBit(UserRole::Owner);

inline constexpr std::array<AuthRouteAccess, 7> kSessionAccess = {{
    {.path = "/auth/sessions", .method = drogon::Get, .roles = kEveryRoleBaseline},
    {.path = "/auth/sessions", .method = drogon::Delete, .roles = kEveryRoleBaseline},
    {.path = "/auth/sessions/{id}", .method = drogon::Delete, .roles = kEveryRoleBaseline},
    {.path = "/auth/users/sessions", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions", .method = drogon::Delete, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions/{id}", .method = drogon::Delete, .roles = kOwnerOnly},
}};

inline constexpr std::array<AuthRouteAccess, 1> kRtcAccess = {{
    {.path = "/rtc/token", .method = drogon::Post, .roles = kEveryRoleBaseline},
}};

inline constexpr std::array<AuthRouteAccess, 1> kSyncAccess = {{
    {.path = "/sync/heartbeat", .method = drogon::Get, .roles = kEveryRoleBaseline},
}};

inline constexpr std::array<AuthRouteAccess, 4> kPrivacyAccess = {{
    {.path = "/privacy/me", .method = drogon::Get, .roles = kEveryRoleBaseline},
    {.path = "/privacy/me", .method = drogon::Put, .roles = kEveryRoleBaseline},
    {.path = "/privacy/users", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/privacy/household", .method = drogon::Patch, .roles = kOwnerOnly},
}};

inline constexpr std::array<AuthRouteAccess, 4> kVisitorAccess = {{
    {.path = "/visitor", .method = drogon::Get, .roles = roleBit(UserRole::Guard)},
    {.path = "/visitor/{id}", .method = drogon::Get, .roles = roleBit(UserRole::Guard)},
    {.path = "/visitor/{id}/crop-preview", .method = drogon::Get, .roles = roleBit(UserRole::Guard)},
    {.path = "/visitor-crop/{id}/content", .method = drogon::Get, .roles = roleBit(UserRole::Guard)},
}};

inline constexpr std::array<AuthRouteAccess, 10> kModuleAccess = {{
    {.path = "/modules", .method = drogon::Get, .roles = kEveryRoleBaseline},
    {.path = "/modules/{id}/install", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/pause", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/resume", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/cancel", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/disable", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/uninstall", .method = drogon::Post, .roles = kOwnerOnly},
    {.path = "/modules/{id}/data", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/modules/{id}/impact", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/modules/{id}/request", .method = drogon::Post, .roles = kNonOwnerBaseline},
}};

inline constexpr std::string_view kSurveillanceModule = "surveillance";
inline constexpr std::string_view kProductivityModule = "productivity";

struct ModuleRoute
{
  std::string_view segment;
  std::string_view module;
};

inline constexpr std::array<ModuleRoute, 12> kModuleRoutes = {{
    {.segment = "camera", .module = kSurveillanceModule},
    {.segment = "zone", .module = kSurveillanceModule},
    {.segment = "media", .module = kSurveillanceModule},
    {.segment = "guard", .module = kSurveillanceModule},
    {.segment = "visitor", .module = kSurveillanceModule},
    {.segment = "visitor-settings", .module = kSurveillanceModule},
    {.segment = "visitor-crop", .module = kSurveillanceModule},
    {.segment = "project", .module = kProductivityModule},
    {.segment = "project-task", .module = kProductivityModule},
    {.segment = "project-member", .module = kProductivityModule},
    {.segment = "calendar-event", .module = kProductivityModule},
    {.segment = "calendar-event-share", .module = kProductivityModule},
}};

inline constexpr std::array<AuthRouteAccess, 1> kRouteOverrides = {{
    {.path = "/notification/delivery-summary", .method = drogon::Get, .roles = kOwnerOnly},
}};

inline constexpr std::string_view kRouteSegment = "{id}";

inline bool routeMatches(std::string_view pattern, std::string_view path)
{
  while (!pattern.empty() || !path.empty()) {
    const auto patternEnd = pattern.find('/', 1);
    const auto pathEnd = path.find('/', 1);
    const std::string_view expected = pattern.substr(0, patternEnd);
    const std::string_view actual = path.substr(0, pathEnd);
    if (expected.empty() || actual.size() < 2)
      return false;
    if (expected.substr(1) == kRouteSegment) {
      if (actual.front() != '/')
        return false;
    } else if (expected != actual) {
      return false;
    }
    pattern.remove_prefix(expected.size());
    path.remove_prefix(actual.size());
  }
  return true;
}

inline bool roleHolds(RoleMask roles, UserRole role)
{
  return (roles & roleBit(role)) != 0;
}

inline bool roleGranted(RoleMask roles, UserRole role, bool roleActive)
{
  return roleHolds(roles, role) && (roleActive || (roles & kBaselineBit) != 0);
}

template <typename Route, std::size_t Count>
inline const Route* routeOf(const std::array<Route, Count>& routes,
                            std::string_view path, drogon::HttpMethod method)
{
  const auto route = std::ranges::find_if(routes, [&](const Route& entry) {
    return entry.method == method && routeMatches(entry.path, path);
  });
  return route == routes.end() ? nullptr : &*route;
}

inline const AuthRouteAccess* sessionRouteOf(std::string_view path,
                                             drogon::HttpMethod method)
{
  return routeOf(kSessionAccess, path, method);
}

inline constexpr std::array<TableName, 7> kBaselineTables = {
    TableName::User,
    TableName::AuditLog,
    TableName::UserAuditLog,
    TableName::Notification,
    TableName::NotificationToken,
    TableName::Reminder,
    TableName::ReminderDetail,
};

inline constexpr std::array<TableName, 2> kOwnRowTables = {
    TableName::Reminder,
    TableName::ReminderDetail,
};

inline bool isBaselineTable(TableName table)
{
  return std::ranges::find(kBaselineTables, table) != kBaselineTables.end();
}

inline bool isOwnRowTable(TableName table)
{
  return std::ranges::find(kOwnRowTables, table) != kOwnRowTables.end();
}

struct TableModule
{
  TableName table;
  std::string_view module;
};

inline constexpr std::array<TableModule, 9> kTableModules = {{
    {.table = TableName::Camera, .module = kSurveillanceModule},
    {.table = TableName::CameraStream, .module = kSurveillanceModule},
    {.table = TableName::Zone, .module = kSurveillanceModule},
    {.table = TableName::Event, .module = kSurveillanceModule},
    {.table = TableName::CalendarEvent, .module = kProductivityModule},
    {.table = TableName::CalendarEventShare, .module = kProductivityModule},
    {.table = TableName::Project, .module = kProductivityModule},
    {.table = TableName::ProjectMember, .module = kProductivityModule},
    {.table = TableName::ProjectTask, .module = kProductivityModule},
}};

inline std::optional<std::string_view> moduleOfTable(TableName table)
{
  const auto entry = std::ranges::find(kTableModules, table, &TableModule::table);
  if (entry == kTableModules.end())
    return std::nullopt;
  return entry->module;
}

struct HasAccessInput
{
  UserRole role;
  TableName table;
  RolePermission perm;
  bool roleActive{true};
};

inline bool hasAccess(const HasAccessInput& input)
{
  const UserRole role = input.role;
  const TableName table = input.table;
  const RolePermission perm = input.perm;

  if (role == UserRole::Owner)
    return true;
  if (!input.roleActive && !isBaselineTable(table))
    return false;

  const auto roleIt = kTableAccess.find(role);
  if (roleIt == kTableAccess.end())
    return false;

  const auto tableIt = roleIt->second.find(table);
  if (tableIt == roleIt->second.end())
    return false;

  return tableIt->second.contains(perm);
}

inline std::vector<TableName> readableTables(UserRole role)
{
  std::vector<TableName> out;
  if (role == UserRole::Owner) {
    for (auto t = static_cast<uint8_t>(TableName::User);
         t <= static_cast<uint8_t>(kLastTableName); ++t) {
      const auto table = static_cast<TableName>(t);
      if (table != TableName::RefreshToken &&
          table != TableName::FaceEmbedding && table != TableName::PersonEvent)
        out.push_back(table);
    }
    return out;
  }

  const auto roleIt = kTableAccess.find(role);
  if (roleIt == kTableAccess.end())
    return out;

  for (const auto& [table, perms] : roleIt->second) {
    if (perms.contains(RolePermission::Read))
      out.push_back(table);
  }
  return out;
}

inline bool readsUserDirectory(UserRole role, bool roleActive = true)
{
  return roleActive && (role == UserRole::Owner || role == UserRole::Guard);
}

inline bool readsCameraConnection(UserRole role)
{
  return role == UserRole::Owner || role == UserRole::Resident;
}

inline std::vector<TableName> moduleTables(UserRole role, bool roleActive = true)
{
  auto tables = readableTables(role);
  if (!readsUserDirectory(role, roleActive))
    std::erase(tables, TableName::User);
  std::erase_if(tables, isOwnRowTable);
  if (!roleActive)
    std::erase_if(tables, [](TableName table) { return !isBaselineTable(table); });
  return tables;
}

inline bool moduleTableActive(TableName table, const ModuleSnapshot& modules)
{
  const auto module = moduleOfTable(table);
  return !module || modules.enabled(*module);
}

inline std::vector<TableName> moduleTables(UserRole role, const ModuleSnapshot& modules)
{
  auto tables = moduleTables(role, modules.roleActive(role));
  std::erase_if(tables, [&](TableName table) { return !moduleTableActive(table, modules); });
  return tables;
}

inline bool tableReadable(UserRole role, TableName table, const ModuleSnapshot& modules)
{
  return moduleTableActive(table, modules) &&
         hasAccess({.role = role,
                    .table = table,
                    .perm = RolePermission::Read,
                    .roleActive = modules.roleActive(role)});
}

inline std::optional<RolePermission> permissionForMethod(drogon::HttpMethod method)
{
  switch (method) {
    case drogon::Get:
      return RolePermission::Read;
    case drogon::Post:
      return RolePermission::Create;
    case drogon::Patch:
      return RolePermission::Update;
    case drogon::Delete:
      return RolePermission::Delete;
    default:
      return std::nullopt;
  }
}

inline std::string_view firstSegment(std::string_view path)
{
  if (path.empty() || path.front() != '/')
    return {};
  path.remove_prefix(1);
  return path.substr(0, path.find('/'));
}

inline std::optional<TableName> tableFromPath(std::string_view path)
{
  static const std::unordered_map<std::string_view, TableName> kSegments = {
      {"camera-stream", TableName::CameraStream},
      {"camera", TableName::Camera},
      {"zone", TableName::Zone},
      {"reminder-detail", TableName::ReminderDetail},
      {"reminder", TableName::Reminder},
      {"calendar-event-share", TableName::CalendarEventShare},
      {"calendar-event", TableName::CalendarEvent},
      {"project-member", TableName::ProjectMember},
      {"project-task", TableName::ProjectTask},
      {"project", TableName::Project},
      {"context-note", TableName::ContextNote},
      {"event", TableName::Event},
      {"person", TableName::Person},
      {"portrait-preview", TableName::User},
      {"invitation", TableName::UserInvitation},
      {"user", TableName::User},
      {"notification-token", TableName::NotificationToken},
      {"notification", TableName::Notification},
  };

  const auto table = kSegments.find(firstSegment(path));
  if (table == kSegments.end())
    return std::nullopt;
  return table->second;
}

inline std::string normalizedPath(std::string_view path)
{
  std::string normalized(path);
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  while (normalized.size() > 1 && normalized.back() == '/')
    normalized.pop_back();
  return normalized;
}

struct HasHttpAccessInput
{
  UserRole role;
  std::string_view path;
  drogon::HttpMethod method;
  bool roleActive{true};
};

template <typename Route, std::size_t Count>
inline bool routeTableAllows(const std::array<Route, Count>& routes,
                             const HasHttpAccessInput& input)
{
  const Route* route = routeOf(routes, input.path, input.method);
  return route != nullptr && roleGranted(route->roles, input.role, input.roleActive);
}

inline bool normalizedRouteAccess(const HasHttpAccessInput& input)
{
  const std::string_view segment = firstSegment(input.path);
  if (segment == "auth")
    return routeTableAllows(kSessionAccess, input);
  if (segment == "rtc")
    return routeTableAllows(kRtcAccess, input);
  if (segment == "privacy")
    return routeTableAllows(kPrivacyAccess, input);
  if (segment == "visitor" || segment == "visitor-crop")
    return routeTableAllows(kVisitorAccess, input);
  if (segment == "sync")
    return routeTableAllows(kSyncAccess, input);
  if (segment == "guard")
    return routeTableAllows(kGuardAccess, input);
  if (segment == "modules")
    return routeTableAllows(kModuleAccess, input);

  if (const auto* route = routeOf(kRouteOverrides, input.path, input.method))
    return roleGranted(route->roles, input.role, input.roleActive);

  if (const auto* action = cameraActionRouteOf(input.path, input.method))
    return roleGranted(action->roles, input.role, input.roleActive);

  const auto table = tableFromPath(input.path);
  const auto perm = permissionForMethod(input.method);
  if (!table || !perm)
    return false;

  return hasAccess({.role = input.role,
                    .table = *table,
                    .perm = *perm,
                    .roleActive = input.roleActive});
}

inline bool hasHttpAccess(const HasHttpAccessInput& input)
{
  if (input.role == UserRole::Owner)
    return true;

  const std::string path = normalizedPath(input.path);
  if (!normalizedRouteAccess({.role = input.role,
                              .path = path,
                              .method = input.method,
                              .roleActive = input.roleActive}))
    return false;
  if (path.size() == input.path.size())
    return true;

  const std::string child = path + "/0";
  return normalizedRouteAccess({.role = input.role,
                                .path = child,
                                .method = input.method,
                                .roleActive = input.roleActive});
}

inline std::optional<std::string_view> moduleOfPath(std::string_view path)
{
  const std::string normalized = normalizedPath(path);
  const std::string_view segment = firstSegment(normalized);
  const auto route = std::ranges::find(kModuleRoutes, segment, &ModuleRoute::segment);
  if (route == kModuleRoutes.end())
    return std::nullopt;
  return route->module;
}

struct CoreRoute
{
  std::string_view path;
  drogon::HttpMethod method;
};

inline constexpr std::array<CoreRoute, 2> kCoreRoutes = {{
    {.path = "/guard/panic", .method = drogon::Post},
    {.path = "/guard/safety", .method = drogon::Get},
}};

inline std::optional<std::string_view> moduleOfRoute(std::string_view path, drogon::HttpMethod method)
{
  const std::string normalized = normalizedPath(path);
  const bool core = std::ranges::any_of(kCoreRoutes, [&](const CoreRoute& route) {
    return route.method == method && routeMatches(route.path, normalized);
  });
  if (core)
    return std::nullopt;
  return moduleOfPath(normalized);
}

enum class RouteVerdict : std::uint8_t
{
  Allowed = 0,
  ModuleDisabled,
  RoleInactive,
  Denied
};

struct RouteInput
{
  UserRole role;
  std::string_view path;
  drogon::HttpMethod method;
  const ModuleSnapshot& modules;
};

inline RouteVerdict routeVerdict(const RouteInput& input)
{
  if (const auto module = moduleOfRoute(input.path, input.method);
      module && !input.modules.enabled(*module))
    return RouteVerdict::ModuleDisabled;
  const bool active = input.modules.roleActive(input.role);
  if (hasHttpAccess({.role = input.role,
                     .path = input.path,
                     .method = input.method,
                     .roleActive = active}))
    return RouteVerdict::Allowed;
  if (!active && userRoleKnown(input.role) &&
      hasHttpAccess({.role = input.role,
                     .path = input.path,
                     .method = input.method,
                     .roleActive = true}))
    return RouteVerdict::RoleInactive;
  return RouteVerdict::Denied;
}

enum class AppAction : std::uint8_t
{
  ShowCamera = 0,
  OpenScreen,
  SetGuardMode
};

inline bool hasAppAction(UserRole role, AppAction action)
{
  switch (action) {
    case AppAction::ShowCamera:
      return hasAccess({.role = role,
                        .table = TableName::Camera,
                        .perm = RolePermission::Read});
    case AppAction::OpenScreen:
      return hasAccess({.role = role,
                        .table = TableName::Notification,
                        .perm = RolePermission::Read});
    case AppAction::SetGuardMode:
      return hasHttpAccess(
          {.role = role, .path = "/guard/mode", .method = drogon::Post});
  }
  return false;
}

}
