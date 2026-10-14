#include "notification-token-index.hxx"

#include "notification-token-query.hxx"

#include <array>
#include <string>
#include <string_view>

namespace notification_token_index
{
namespace
{
struct AdditiveColumn
{
  std::string_view name;
  std::string_view definition;
};

constexpr std::array kAdditiveColumns{
    AdditiveColumn{"session_id", "TEXT NOT NULL DEFAULT ''"},
    AdditiveColumn{"provider", "TEXT NOT NULL DEFAULT ''"},
    AdditiveColumn{"public_key", "TEXT NOT NULL DEFAULT ''"},
    AdditiveColumn{"key_id", "TEXT NOT NULL DEFAULT ''"}};

std::string columnCountSql(std::string_view column)
{
  std::string sql = "SELECT COUNT(*) FROM pragma_table_info('notification_token') WHERE name = '";
  sql += column;
  sql += "'";
  return sql;
}

std::string addColumnSql(const AdditiveColumn& column)
{
  std::string sql = "ALTER TABLE notification_token ADD COLUMN ";
  sql += column.name;
  sql += ' ';
  sql += column.definition;
  return sql;
}
}

TokenMigrationResult migrate(TokenIndexSql& sql)
{
  TokenMigrationResult result;
  for (const auto& column : kAdditiveColumns) {
    if (sql.countRows(columnCountSql(column.name)) == 0) {
      sql.execute(addColumnSql(column));
      ++result.columnsAdded;
    }
  }
  if (sql.countRows(notification_token_query::LEGACY_INDEX_COUNT) > 0) {
    sql.execute(notification_token_query::DROP_LEGACY_INDEX);
    result.droppedLegacyIndex = true;
  }
  sql.execute(notification_token_query::CREATE_DEVICE_INDEX);
  return result;
}
}
