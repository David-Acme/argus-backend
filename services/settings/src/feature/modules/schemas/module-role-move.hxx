#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ModuleRoleMove
{
  std::int64_t userId{0};
  std::string name;
  std::string from;
  std::string to;
};

struct ModuleRoleMoveNote
{
  std::string es;
  std::string en;
};

struct ModuleAuditDetail
{
  std::string data;
  std::vector<ModuleRoleMove> moves;
};

namespace module_role_move
{
[[nodiscard]] std::string detailOf(const ModuleAuditDetail& detail);
[[nodiscard]] ModuleAuditDetail parse(std::string_view detail);
[[nodiscard]] ModuleRoleMoveNote noteOf(const std::vector<ModuleRoleMove>& moves);
}
