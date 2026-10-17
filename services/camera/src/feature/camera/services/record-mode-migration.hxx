#pragma once

#include <sqlite/table-rebuild.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace record_mode_migration
{
inline constexpr std::string_view kCameraTable = "camera";

struct Input
{
  sqlite3* db;
  std::string backupPath;
};

[[nodiscard]] std::optional<std::string> withWidenedCheck(std::string_view definition);
[[nodiscard]] table_rebuild::Outcome apply(const Input& input);
[[nodiscard]] std::string backupPathFor(const std::string& dbPath, std::int64_t unixSeconds);
[[nodiscard]] std::string freeBackupPath(const std::string& path);
[[nodiscard]] bool applyToFile(const std::string& dbPath);
}
