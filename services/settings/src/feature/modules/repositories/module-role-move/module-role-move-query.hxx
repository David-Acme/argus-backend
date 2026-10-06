#pragma once

#include <feature/modules/schemas/module-role-move.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace module_role_move_query
{
inline constexpr const char* INSERT =
    "INSERT INTO module_role_move (job_id, module_id, user_id, user_name, from_role, to_role, actor_user_id, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
inline constexpr const char* SELECT_ALL =
    "SELECT job_id, user_id, user_name, from_role, to_role FROM module_role_move ORDER BY id";
}

struct ModuleRoleMoveCreateInput
{
  std::int64_t jobId{0};
  std::string moduleId;
  std::int64_t actorUserId{0};
  std::vector<ModuleRoleMove> moves;
  std::int64_t at{0};
};

struct ModuleRoleMoveRow
{
  std::int64_t jobId{0};
  ModuleRoleMove move;
};
