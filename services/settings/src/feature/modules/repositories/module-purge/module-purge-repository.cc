#include "module-purge-repository.hxx"

#include <utility>

using namespace module_purge_query;

ModulePurgeRepository::ModulePurgeRepository(drogon::orm::DbClientPtr client) : client_(std::move(client)) {}

std::set<std::string> ModulePurgeRepository::purgedOwners(const std::string& moduleId) const
{
  std::set<std::string> owners;
  for (const auto& row : client_->execSqlSync(SELECT_OWNERS, moduleId))
    owners.insert(row["owner"].as<std::string>());
  return owners;
}

void ModulePurgeRepository::create(const ModulePurgeCreateInput& input) const
{
  client_->execSqlSync(INSERT, input.moduleId, input.owner, input.at);
}

void ModulePurgeRepository::clear(const std::string& moduleId) const
{
  client_->execSqlSync(DELETE_MODULE, moduleId);
}
