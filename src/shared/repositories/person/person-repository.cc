#include "person-repository.hxx"
#include <shared/services/sqlite/db-service.hxx>

using namespace person_query;

drogon::Task<std::optional<PersonSchema>>
PersonRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty()) co_return std::nullopt;
  co_return PersonSchema(result.front());
}

drogon::Task<std::vector<PersonSchema>>
PersonRepository::findByUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_USER.data(), userId);

  std::vector<PersonSchema> data;
  for (const auto& row : result)
    data.push_back(PersonSchema(row));
  co_return data;
}

drogon::Task<PersonSchema>
PersonRepository::create(const PersonCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
      input.userId ? *input.userId : std::optional<int64_t>{},
      input.name, input.alias, input.observation);

  auto created = co_await findById(result.insertId());
  if (!created) {
    LOG_WARN << "Person not found after insert";
    co_return {};
  }
  co_return *created;
}

drogon::Task<PersonSchema>
PersonRepository::update(int64_t id,
                         const PersonUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPDATE.data(), input.name, input.alias,
                               input.observation, id);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "Person not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> PersonRepository::linkUser(int64_t id,
                                               int64_t userId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPDATE_USER.data(), userId, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> PersonRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}
