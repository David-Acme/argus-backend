#include "person-tag-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>

using namespace person_tag_query;

drogon::Task<int> PersonTagRepository::addMany(
    const PersonTagAddInput& input) const
{
  int added = 0;
  for (const auto& tag : input.tags) {
    if (tag.empty())
      continue;
    auto client = DbService::client();
    const auto result = co_await client->execSqlCoro(INSERT.data(),
                                                     input.personId, tag,
                                                     input.source);
    added += static_cast<int>(result.affectedRows());
  }
  co_return added;
}

drogon::Task<std::vector<std::string>>
PersonTagRepository::findByPerson(int64_t personId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_PERSON.data(),
                                                   personId);
  std::vector<std::string> tags;
  tags.reserve(result.size());
  for (const auto& row : result)
    tags.push_back(row["tag"].as<std::string>());
  co_return tags;
}
