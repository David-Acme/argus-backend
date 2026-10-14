#pragma once

#include <shared/repositories/notification-token/notification-token-index.hxx>

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
class SqliteTokenIndexSql final : public TokenIndexSql
{
public:
  explicit SqliteTokenIndexSql(sqlite3* db) : db_(db) {}

  int64_t countRows(std::string_view sql) override
  {
    sqlite3_stmt* statement = nullptr;
    const std::string text(sql);
    if (sqlite3_prepare_v2(db_, text.c_str(), -1, &statement, nullptr) != SQLITE_OK)
      return -1;
    int64_t value = 0;
    if (sqlite3_step(statement) == SQLITE_ROW)
      value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
  }

  void execute(std::string_view sql) override
  {
    const std::string text(sql);
    lastStatus = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, nullptr);
  }

  int lastStatus{SQLITE_OK};

private:
  sqlite3* db_;
};
}
