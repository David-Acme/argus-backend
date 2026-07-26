#include "context-note-repository.hxx"
#include <shared/services/sqlite/db-service.hxx>

using namespace context_note_query;

drogon::Task<std::optional<ContextNoteSchema>>
ContextNoteRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty()) co_return std::nullopt;
  co_return ContextNoteSchema(result.front());
}

drogon::Task<std::vector<ContextNoteSchema>>
ContextNoteRepository::findActive() const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_ACTIVE.data());

  std::vector<ContextNoteSchema> data;
  for (const auto& row : result)
    data.push_back(ContextNoteSchema(row));
  co_return data;
}

drogon::Task<ContextNoteSchema>
ContextNoteRepository::create(const ContextNoteCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
      input.createdBy ? *input.createdBy : std::optional<int64_t>{},
      input.title, input.content, input.tags,
      input.validFrom ? *input.validFrom : std::optional<int64_t>{},
      input.validUntil ? *input.validUntil : std::optional<int64_t>{});

  auto created = co_await findById(result.insertId());
  if (!created) {
    LOG_WARN << "ContextNote not found after insert";
    co_return {};
  }
  co_return *created;
}

drogon::Task<ContextNoteSchema>
ContextNoteRepository::update(int64_t id,
                              const ContextNoteUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      UPDATE.data(), input.title, input.content, input.tags,
      input.validFrom ? *input.validFrom : std::optional<int64_t>{},
      input.validUntil ? *input.validUntil : std::optional<int64_t>{},
      input.isActive ? 1 : 0, id);

  auto updated = co_await findById(id);
  if (!updated) {
    LOG_WARN << "ContextNote not found after update";
    co_return {};
  }
  co_return *updated;
}

drogon::Task<bool> ContextNoteRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}
