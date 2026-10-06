#include "role-check-migration.hxx"

#include <drogon/drogon.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <regex>
#include <system_error>

namespace
{
constexpr int kBusyTimeoutMs = 5000;
constexpr unsigned kOpenFlags = static_cast<unsigned>(SQLITE_OPEN_READWRITE) | static_cast<unsigned>(SQLITE_OPEN_URI);
constexpr std::string_view kUriPrefix = "file:";
constexpr std::string_view kMemoryDatabase = ":memory:";

bool isMemoryDatabase(std::string_view path)
{
  return path.empty() || path == kMemoryDatabase || path.find("mode=memory") != std::string_view::npos;
}
}

namespace role_check_migration
{
std::optional<std::string> withoutRoleCheck(std::string_view definition)
{
  static const std::regex kRoleCheck(R"(\s*CHECK\s*\(\s*role\s+IN\s*\([^)]*\)\s*\))", std::regex::icase);
  const std::string text(definition);
  if (!std::regex_search(text, kRoleCheck))
    return std::nullopt;
  return std::regex_replace(text, kRoleCheck, "", std::regex_constants::format_first_only);
}

table_rebuild::Outcome apply(const Input& input)
{
  return table_rebuild::run({.db = input.db,
                             .tables = {std::string(kUserTable), std::string(kInvitationTable)},
                             .rewrite = [](std::string_view definition) { return withoutRoleCheck(definition); },
                             .backupPath = input.backupPath});
}

std::string backupPathFor(const std::string& dbPath, std::int64_t unixSeconds)
{
  if (isMemoryDatabase(dbPath) || dbPath.starts_with(kUriPrefix))
    return {};
  return dbPath + ".role-rebuild-" + std::to_string(unixSeconds) + ".bak";
}

std::string freeBackupPath(const std::string& path)
{
  if (path.empty())
    return path;
  std::error_code ignored;
  std::string candidate = path;
  for (int attempt = 1; std::filesystem::exists(candidate, ignored); ++attempt)
    candidate = path + "-" + std::to_string(attempt);
  return candidate;
}

bool applyToFile(const std::string& dbPath)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(dbPath.c_str(), &raw, static_cast<int>(kOpenFlags), nullptr) != SQLITE_OK) {
    LOG_ERROR << "Identity roles: could not open " << dbPath << " for the role check migration";
    sqlite3_close(raw);
    return false;
  }
  sqlite3_busy_timeout(raw, kBusyTimeoutMs);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto outcome = apply({.db = raw, .backupPath = freeBackupPath(backupPathFor(dbPath, now))});
  sqlite3_close(raw);
  if (!outcome.ok()) {
    LOG_ERROR << "Identity roles: the role check migration failed and changed nothing: " << outcome.error;
    return false;
  }
  if (!outcome.rebuilt.empty())
    LOG_INFO << "Identity roles: the role CHECK constraints were dropped from " << outcome.rebuilt.size()
             << " tables (backup " << (outcome.backup.empty() ? "none" : outcome.backup) << ")";
  return true;
}
}
