#include "clip-trigger-migration.hxx"

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

std::string backupPathFor(const std::string& dbPath, std::int64_t unixSeconds)
{
  if (isMemoryDatabase(dbPath) || dbPath.starts_with(kUriPrefix))
    return {};
  std::string candidate = dbPath + ".clip-trigger-rebuild-" + std::to_string(unixSeconds) + ".bak";
  std::error_code ignored;
  const std::string base = candidate;
  for (int attempt = 1; std::filesystem::exists(candidate, ignored); ++attempt)
    candidate = base + "-" + std::to_string(attempt);
  return candidate;
}
}

namespace clip_trigger_migration
{
std::optional<std::string> withWidenedCheck(std::string_view definition)
{
  static const std::regex kTriggerCheck(R"(\s*CHECK\s*\(\s*trigger\s+IN\s*\([^)]*\)\s*\))", std::regex::icase);
  const std::string text(definition);
  std::smatch found;
  if (!std::regex_search(text, found, kTriggerCheck))
    return std::nullopt;
  const std::string current = found.str();
  if (current.find("'tamper'") != std::string::npos && current.find("'look'") != std::string::npos)
    return std::nullopt;
  const std::string widened = " CHECK (trigger IN ('motion', 'event', 'continuous', 'tamper', 'look'))";
  std::string rewritten =
      std::regex_replace(text, kTriggerCheck, widened, std::regex_constants::format_first_only);
  if (rewritten == text)
    return std::nullopt;
  return rewritten;
}

table_rebuild::Outcome apply(const Input& input)
{
  return table_rebuild::run({.db = input.db,
                             .tables = {std::string(kClipTable)},
                             .rewrite = [](std::string_view definition) { return withWidenedCheck(definition); },
                             .backupPath = input.backupPath});
}

bool applyToFile(const std::string& dbPath)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(dbPath.c_str(), &raw, static_cast<int>(kOpenFlags), nullptr) != SQLITE_OK) {
    LOG_ERROR << "Camera clips: could not open " << dbPath << " for the clip trigger migration";
    sqlite3_close(raw);
    return false;
  }
  sqlite3_busy_timeout(raw, kBusyTimeoutMs);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto outcome = apply({.db = raw, .backupPath = backupPathFor(dbPath, now)});
  sqlite3_close(raw);
  if (!outcome.ok()) {
    LOG_ERROR << "Camera clips: the clip trigger migration failed and changed nothing: " << outcome.error;
    return false;
  }
  if (!outcome.rebuilt.empty())
    LOG_INFO << "Camera clips: the trigger CHECK now admits tamper and look (backup "
             << (outcome.backup.empty() ? "none" : outcome.backup) << ")";
  return true;
}
}
