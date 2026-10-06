#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace table_rebuild
{
using Rewrite = std::function<std::optional<std::string>(std::string_view definition)>;

struct Input
{
  sqlite3* db;
  std::vector<std::string> tables;
  Rewrite rewrite;
  std::string backupPath;
};

struct Outcome
{
  std::vector<std::string> rebuilt;
  std::string backup;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

[[nodiscard]] Outcome run(const Input& input);
}
