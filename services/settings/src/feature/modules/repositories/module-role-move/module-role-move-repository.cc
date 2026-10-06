#include "module-role-move-repository.hxx"

#include <utility>

using namespace module_role_move_query;

ModuleRoleMoveRepository::ModuleRoleMoveRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

void ModuleRoleMoveRepository::create(const ModuleRoleMoveCreateInput& input) const
{
  for (const auto& move : input.moves)
    client_->execSqlSync(INSERT, input.jobId, input.moduleId, move.userId, move.name, move.from, move.to,
                         input.actorUserId, input.at);
}

std::vector<ModuleRoleMoveRow> ModuleRoleMoveRepository::findAll() const
{
  std::vector<ModuleRoleMoveRow> rows;
  for (const auto& row : client_->execSqlSync(SELECT_ALL))
    rows.push_back({.jobId = row["job_id"].as<std::int64_t>(),
                    .move = {.userId = row["user_id"].as<std::int64_t>(),
                             .name = row["user_name"].as<std::string>(),
                             .from = row["from_role"].as<std::string>(),
                             .to = row["to_role"].as<std::string>()}});
  return rows;
}
