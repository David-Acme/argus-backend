#pragma once

#include <span>
#include <string_view>

struct ColumnMigration
{
  std::string_view table;
  std::string_view column;
  std::string_view definition;
};

namespace column_migration
{
void ensure(std::span<const ColumnMigration> columns);
}
