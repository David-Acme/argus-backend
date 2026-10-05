#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <drogon/HttpTypes.h>
#include <optional>
#include <sync/role-permission.hxx>
#include <sync/table-name.hxx>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <auth/user-role.hxx>
#include <vector>

namespace role_access
{

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
      {TableName::User, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
};

inline const std::unordered_map<UserRole,
                                std::unordered_set<drogon::HttpMethod>>
    kAuthAccess = {
        {UserRole::Resident, {drogon::Get, drogon::Post, drogon::Patch}},
        {UserRole::Guard, {drogon::Get}},
        {UserRole::Guest, {drogon::Get}},
};

struct GuardRouteAccess
{
  std::string_view path;
  drogon::HttpMethod method;
  std::uint8_t roles;
};

constexpr std::uint8_t roleBit(UserRole role)
{
  return static_cast<std::uint8_t>(1U << static_cast<unsigned>(role));
}

inline constexpr std::uint8_t kResidentAndGuard = roleBit(UserRole::Resident) | roleBit(UserRole::Guard);

inline constexpr std::array<GuardRouteAccess, 7> kGuardAccess = {{
    {.path = "/guard/environments", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/episodes", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/mode", .method = drogon::Post, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/incidents", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/expected-guests", .method = drogon::Get, .roles = kResidentAndGuard},
    {.path = "/guard/expected-guests", .method = drogon::Post, .roles = roleBit(UserRole::Resident)},
    {.path = "/guard/expected-guests", .method = drogon::Delete, .roles = roleBit(UserRole::Resident)},
}};

enum class CameraAction : std::uint8_t
{
  Talk = 0
};

struct CameraActionAccess
{
  CameraAction action;
  std::string_view segment;
  drogon::HttpMethod method;
  std::uint8_t roles;
};

inline constexpr std::array<CameraActionAccess, 1> kCameraActionAccess = {{
    {.action = CameraAction::Talk, .segment = "talk", .method = drogon::Post, .roles = kResidentAndGuard},
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
  std::uint8_t roles;
};

inline constexpr std::uint8_t kEveryRole =
    roleBit(UserRole::Owner) | roleBit(UserRole::Resident) |
    roleBit(UserRole::Guard) | roleBit(UserRole::Guest);

inline constexpr std::uint8_t kOwnerOnly = roleBit(UserRole::Owner);

inline constexpr std::array<AuthRouteAccess, 7> kSessionAccess = {{
    {.path = "/auth/sessions", .method = drogon::Get, .roles = kEveryRole},
    {.path = "/auth/sessions", .method = drogon::Delete, .roles = kEveryRole},
    {.path = "/auth/sessions/{id}", .method = drogon::Delete, .roles = kEveryRole},
    {.path = "/auth/users/sessions", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions", .method = drogon::Get, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions", .method = drogon::Delete, .roles = kOwnerOnly},
    {.path = "/auth/users/{id}/sessions/{id}", .method = drogon::Delete, .roles = kOwnerOnly},
}};

inline constexpr std::array<AuthRouteAccess, 1> kRtcAccess = {{
    {.path = "/rtc/token", .method = drogon::Post, .roles = kEveryRole},
}};

inline constexpr std::array<AuthRouteAccess, 1> kSyncAccess = {{
    {.path = "/sync/heartbeat", .method = drogon::Get, .roles = kEveryRole},
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

inline const AuthRouteAccess* sessionRouteOf(std::string_view path,
                                             drogon::HttpMethod method)
{
  const auto route =
      std::ranges::find_if(kSessionAccess, [&](const AuthRouteAccess& entry) {
        return entry.method == method && routeMatches(entry.path, path);
      });
  return route == kSessionAccess.end() ? nullptr : &*route;
}

struct HasAccessInput
{
  UserRole role;
  TableName table;
  RolePermission perm;
};

inline bool hasAccess(const HasAccessInput& input)
{
  const UserRole role = input.role;
  const TableName table = input.table;
  const RolePermission perm = input.perm;

  if (role == UserRole::Owner)
    return true;

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

inline bool readsUserDirectory(UserRole role)
{
  return role == UserRole::Owner || role == UserRole::Guard;
}

inline std::vector<TableName> moduleTables(UserRole role)
{
  auto tables = readableTables(role);
  if (!readsUserDirectory(role))
    std::erase(tables, TableName::User);
  return tables;
}

inline RolePermission permissionForMethod(drogon::HttpMethod method)
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
      return RolePermission::Read;
  }
}

inline std::optional<TableName> tableFromPath(std::string_view path)
{
  static const std::vector<std::pair<std::string_view, TableName>> kPaths = {
      {"/camera-stream", TableName::CameraStream},
      {"/camera", TableName::Camera},
      {"/zone", TableName::Zone},
      {"/reminder-detail", TableName::ReminderDetail},
      {"/reminder", TableName::Reminder},
      {"/calendar-event-share", TableName::CalendarEventShare},
      {"/calendar-event", TableName::CalendarEvent},
      {"/project-member", TableName::ProjectMember},
      {"/project-task", TableName::ProjectTask},
      {"/project", TableName::Project},
      {"/context-note", TableName::ContextNote},
      {"/event", TableName::Event},
      {"/person", TableName::Person},
      {"/portrait-preview", TableName::User},
      {"/invitation", TableName::UserInvitation},
      {"/user", TableName::User},
      {"/notification-token", TableName::NotificationToken},
      {"/notification", TableName::Notification},
  };

  for (const auto& [prefix, table] : kPaths) {
    if (path.rfind(prefix, 0) == 0)
      return table;
  }
  return std::nullopt;
}

struct HasHttpAccessInput
{
  UserRole role;
  std::string_view path;
  drogon::HttpMethod method;
};

inline bool hasHttpAccess(const HasHttpAccessInput& input)
{
  const UserRole role = input.role;
  const std::string_view path = input.path;
  const drogon::HttpMethod method = input.method;

  if (role == UserRole::Owner)
    return true;

  if (path.rfind("/auth", 0) == 0) {
    if (const auto* route = sessionRouteOf(path, method))
      return (route->roles & roleBit(role)) != 0;
    const auto it = kAuthAccess.find(role);
    if (it == kAuthAccess.end())
      return false;
    return it->second.contains(method);
  }

  if (path.rfind("/rtc", 0) == 0) {
    const auto route = std::ranges::find_if(kRtcAccess, [&](const AuthRouteAccess& entry) {
      return entry.path == path && entry.method == method;
    });
    return route != kRtcAccess.end() && (route->roles & roleBit(role)) != 0;
  }

  if (path.starts_with("/sync/")) {
    const auto route = std::ranges::find_if(kSyncAccess, [&](const AuthRouteAccess& entry) {
      return entry.path == path && entry.method == method;
    });
    return route != kSyncAccess.end() && (route->roles & roleBit(role)) != 0;
  }

  if (path.rfind("/guard", 0) == 0) {
    const auto route = std::ranges::find_if(kGuardAccess, [&](const GuardRouteAccess& entry) {
      return entry.path == path && entry.method == method;
    });
    return route != kGuardAccess.end() && (route->roles & roleBit(role)) != 0;
  }

  if (const auto* action = cameraActionRouteOf(path, method))
    return (action->roles & roleBit(role)) != 0;

  const auto table = tableFromPath(path);
  if (!table)
    return false;

  return hasAccess(
      {.role = role, .table = *table, .perm = permissionForMethod(method)});
}

}
