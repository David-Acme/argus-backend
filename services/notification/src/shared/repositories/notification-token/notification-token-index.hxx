#pragma once

#include <cstdint>
#include <string_view>

class TokenIndexSql
{
public:
  virtual ~TokenIndexSql() = default;

  virtual int64_t countRows(std::string_view sql) = 0;
  virtual void execute(std::string_view sql) = 0;
};

namespace notification_token_index
{
struct TokenMigrationResult
{
  int columnsAdded{0};
  bool droppedLegacyIndex{false};

  [[nodiscard]] bool changed() const noexcept
  {
    return columnsAdded > 0 || droppedLegacyIndex;
  }
};

[[nodiscard]] TokenMigrationResult migrate(TokenIndexSql& sql);
}
