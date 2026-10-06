#pragma once

#include <feature/modules/schemas/module-audit.hxx>

#include <cstdint>
#include <string>

namespace module_audit_query
{
inline constexpr const char* INSERT =
    "INSERT INTO module_audit (module_id, action, user_id, detail, created_at) VALUES (?, ?, ?, ?, ?)";
inline constexpr const char* SELECT_ENABLED_VERSION =
    "SELECT COALESCE(MAX(id), 0) AS version FROM module_audit "
    "WHERE action IN ('adopted', 'enabled', 'disabled', 'rolled_back', 'removed', 'purged')";
inline constexpr const char* SELECT_LAST_ID = "SELECT COALESCE(MAX(id), 0) AS last FROM module_audit";
inline constexpr const char* SELECT_AFTER =
    "SELECT id, module_id, action, user_id, detail, created_at FROM module_audit WHERE id > ? ORDER BY id LIMIT ?";
inline constexpr const char* SELECT_BY_MODULE =
    "SELECT id, module_id, action, user_id, detail, created_at FROM module_audit WHERE module_id = ? ORDER BY id";
}

struct ModuleAuditAfterInput
{
  std::int64_t afterId{0};
  int limit{50};
};

struct ModuleAuditCreateInput
{
  std::string moduleId;
  ModuleAuditAction action{ModuleAuditAction::InstallRequested};
  std::int64_t userId{0};
  std::string detail;
  std::int64_t at{0};
};

struct ModuleAuditSchema
{
  std::int64_t id{0};
  std::string moduleId;
  ModuleAuditAction action{ModuleAuditAction::InstallRequested};
  std::int64_t userId{0};
  std::string detail;
  std::int64_t createdAt{0};
};
