#include "context-note-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <vector>

using namespace context_note_query;

drogon::Task<std::optional<ContextNoteSchema>>
ContextNoteRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return ContextNoteSchema(result.front());
}

drogon::Task<std::vector<ContextNoteSchema>>
ContextNoteRepository::findActive() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_ACTIVE.data());

  std::vector<ContextNoteSchema> data;
  for (const auto& row : result)
    data.push_back(ContextNoteSchema(row));
  co_return data;
}

drogon::Task<ContextNoteSchema>
ContextNoteRepository::create(const ContextNoteCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(),
                                   input.createdBy ? *input.createdBy
                                                   : std::optional<int64_t>{},
                                   input.title, input.content, input.tags,
                                   input.validFrom ? *input.validFrom
                                                   : std::optional<int64_t>{},
                                   input.validUntil ? *input.validUntil
                                                    : std::optional<int64_t>{});

  ContextNoteSchema schema;
  schema.id = result.insertId();
  schema.createdBy = input.createdBy;
  schema.title = input.title;
  schema.content = input.content;
  schema.tags = input.tags;
  schema.validFrom = input.validFrom;
  schema.validUntil = input.validUntil;
  schema.isActive = true;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<ContextNoteSchema>
ContextNoteRepository::update(int64_t id,
                              const ContextNoteUpdateInput& input) const
{
  auto client = DbService::client();
  std::string sql = UPDATE_PREFIX.data();
  std::vector<std::string> args;

  auto addString = [&](std::string_view column,
                       const std::optional<std::string>& value) {
    if (!value)
      return;
    if (!args.empty())
      sql += ", ";
    sql += column;
    args.push_back(*value);
  };

  addString(UPDATE_COL_TITLE, input.title);
  addString(UPDATE_COL_CONTENT, input.content);
  addString(UPDATE_COL_TAGS, input.tags);
  if (input.validFrom) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_VALID_FROM;
    args.push_back(std::to_string(*input.validFrom));
  }
  if (input.validUntil) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_VALID_UNTIL;
    args.push_back(std::to_string(*input.validUntil));
  }
  if (input.isActive) {
    if (!args.empty())
      sql += ", ";
    sql += UPDATE_COL_IS_ACTIVE;
    args.push_back(*input.isActive ? "1" : "0");
  }

  if (args.empty()) {
    auto existing = co_await findById(id);
    if (!existing) {
      LOG_WARN << "ContextNote not found for update";
      co_return {};
    }
    co_return *existing;
  }

  sql += UPDATE_SUFFIX.data();
  args.push_back(std::to_string(id));
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);

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
