#include "face-upgrade-repository.hxx"

#include <sqlite/db-service.hxx>
#include <string>

using namespace face_upgrade_query;

drogon::Task<std::vector<FaceUpgradeCandidate>>
FaceUpgradeRepository::findStaleUserPersons(std::string_view model) const
{
  const std::string current(model);
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(FIND_STALE_USER_PERSONS), current, current);
  std::vector<FaceUpgradeCandidate> candidates;
  candidates.reserve(result.size());
  for (const auto& row : result)
    candidates.push_back(
        {.personId = row["person_id"].as<int64_t>(),
         .userId = row["user_id"].as<int64_t>()});
  co_return candidates;
}
