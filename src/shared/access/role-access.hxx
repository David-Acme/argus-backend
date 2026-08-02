#pragma once

#include <drogon/HttpTypes.h>
#include <optional>
#include <shared/enums.hxx>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class RolePermission : uint8_t
{
  Read = 0,
  Create,
  Update,
  Delete
};

namespace role_access
{

using PermSet = std::unordered_set<RolePermission>;
using TableAccess = std::unordered_map<TableName, PermSet>;

inline const PermSet kRead{RolePermission::Read};
inline const PermSet kReadUpdate{RolePermission::Read, RolePermission::Update};
inline const PermSet kCreateOwn{RolePermission::Create};
inline const PermSet kFull{RolePermission::Read, RolePermission::Create,
                           RolePermission::Update, RolePermission::Delete};

// Permisos por tabla (recursos). Owner tiene acceso total y no se lista aquí.
inline const std::unordered_map<UserRole, TableAccess> kTableAccess = {
    {UserRole::Resident,
     {{TableName::Camera, kFull},
      {TableName::CameraStream, kFull},
      {TableName::Zone, kFull},
      {TableName::Reminder, kFull},
      {TableName::ReminderDetail, kFull},
      {TableName::Event, kFull},
      {TableName::Person, kFull},
      {TableName::ContextNote, kFull},
      {TableName::User, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
    {UserRole::Guard,
     {{TableName::Camera, kRead},
      {TableName::CameraStream, kRead},
      {TableName::Event, kRead},
      {TableName::Person, kRead},
      {TableName::Zone, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
    {UserRole::Guest,
     {{TableName::Camera, kRead},
      {TableName::AuditLog, kRead},
      {TableName::UserAuditLog, kRead},
      {TableName::Notification, kReadUpdate},
      {TableName::NotificationToken, kCreateOwn}}},
};

// Rutas de auth (no son tablas) → métodos permitidos por rol.
inline const std::unordered_map<UserRole, std::unordered_set<drogon::HttpMethod>>
    kAuthAccess = {
        {UserRole::Resident, {drogon::Get, drogon::Post, drogon::Patch}},
        {UserRole::Guard, {drogon::Get}},
        {UserRole::Guest, {drogon::Get}},
};

inline bool hasAccess(UserRole role, TableName table, RolePermission perm)
{
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
         t <= static_cast<uint8_t>(TableName::FaceEmbedding); ++t) {
      const auto table = static_cast<TableName>(t);
      if (table != TableName::RefreshToken && table != TableName::FaceEmbedding &&
          table != TableName::PersonEvent)
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

// Los prefijos más específicos deben ir primero (camera-stream antes que camera).
inline std::optional<TableName> tableFromPath(std::string_view path)
{
  static const std::vector<std::pair<std::string_view, TableName>> kPaths = {
      {"/camera-stream", TableName::CameraStream},
      {"/camera", TableName::Camera},
      {"/zone", TableName::Zone},
      {"/reminder-detail", TableName::ReminderDetail},
      {"/reminder", TableName::Reminder},
      {"/context-note", TableName::ContextNote},
      {"/event", TableName::Event},
      {"/person", TableName::Person},
      {"/notification-token", TableName::NotificationToken},
      {"/notification", TableName::Notification},
  };

  for (const auto& [prefix, table] : kPaths) {
    if (path.rfind(prefix, 0) == 0)
      return table;
  }
  return std::nullopt;
}

inline bool hasHttpAccess(UserRole role, std::string_view path,
                          drogon::HttpMethod method)
{
  if (role == UserRole::Owner)
    return true;

  if (path.rfind("/auth", 0) == 0) {
    const auto it = kAuthAccess.find(role);
    if (it == kAuthAccess.end())
      return false;
    return it->second.contains(method);
  }

  const auto table = tableFromPath(path);
  if (!table)
    return false;

  return hasAccess(role, *table, permissionForMethod(method));
}

} // namespace role_access
