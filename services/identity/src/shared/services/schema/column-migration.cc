#include "column-migration.hxx"

#include <drogon/drogon.h>
#include <sqlite/db-service.hxx>
#include <string>

void column_migration::ensure(std::span<const ColumnMigration> columns)
{
  const auto client = DbService::client();
  for (const auto& column : columns) {
    const auto found = client->execSqlSync(
        "SELECT COUNT(*) AS total FROM pragma_table_info(?) WHERE name = ?",
        std::string(column.table), std::string(column.column));
    if (!found.empty() && found.front()["total"].as<int>() > 0)
      continue;
    client->execSqlSync("ALTER TABLE " + std::string(column.table) +
                        " ADD COLUMN " + std::string(column.column) + " " +
                        std::string(column.definition));
    LOG_INFO << "Identity schema: added " << column.table << "."
             << column.column;
  }
}
