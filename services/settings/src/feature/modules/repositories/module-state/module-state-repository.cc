#include "module-state-repository.hxx"

#include <utility>

using namespace module_state_query;

ModuleStateRepository::ModuleStateRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

std::vector<ModuleStateSchema> ModuleStateRepository::findAll() const
{
  const auto rows = client_->execSqlSync(SELECT_ALL);
  std::vector<ModuleStateSchema> states;
  states.reserve(rows.size());
  for (const auto& row : rows)
    states.push_back({.moduleId = row["module_id"].as<std::string>(),
                      .lifecycle = moduleLifecycleFromString(row["lifecycle"].as<std::string>())
                                       .value_or(ModuleLifecycle::NotInstalled),
                      .dataPurgedAt = row["data_purged_at"].as<std::int64_t>(),
                      .updatedAt = row["updated_at"].as<std::int64_t>()});
  return states;
}

ModuleStateSchema ModuleStateRepository::upsert(const ModuleStateUpsertInput& input) const
{
  client_->execSqlSync(UPSERT, input.moduleId, std::string(moduleLifecycleToString(input.lifecycle)),
                       input.dataPurgedAt, input.at, input.at, input.dataPurgedAt);
  return {.moduleId = input.moduleId,
          .lifecycle = input.lifecycle,
          .dataPurgedAt = input.dataPurgedAt.value_or(0),
          .updatedAt = input.at};
}
