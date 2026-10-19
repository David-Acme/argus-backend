#pragma once

#include <sqlite/table-rebuild.hxx>

#include <optional>
#include <string>
#include <string_view>

namespace clip_trigger_migration
{
inline constexpr std::string_view kClipTable = "camera_clip";

struct Input
{
  sqlite3* db;
  std::string backupPath;
};

[[nodiscard]] std::optional<std::string> withWidenedCheck(std::string_view definition);
[[nodiscard]] table_rebuild::Outcome apply(const Input& input);
[[nodiscard]] bool applyToFile(const std::string& dbPath);
}
