#include "identity-migration.hxx"

#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>

namespace
{

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

const std::vector<std::string> kIdentityTables = {
    "user",
    "person",
    "face_embedding",
    "refresh_token",
    "device_login_challenge",
    "user_invitation",
    "invitation_redemption",
};

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct IdentityHandleResult
{
  bool ok = false;
  DbHandle db{nullptr, sqlite3_close_v2};
  std::string error;
};

struct IdentityExecInput
{
  sqlite3* db = nullptr;
  std::string sql;
};

struct IdentityChecksumInput
{
  sqlite3* db = nullptr;
  std::string schema;
  std::string table;
};

struct IdentityChecksumResult
{
  bool ok = false;
  int64_t rows = 0;
  std::string checksum;
  std::string error;
};

struct IdentityColumnShapeInput
{
  sqlite3* db = nullptr;
  std::string schema;
  std::string table;
};

std::vector<std::string> splitStatements(const std::string& script)
{
  std::vector<std::string> statements;
  std::string current;
  std::istringstream ss(script);
  std::string line;
  while (std::getline(ss, line)) {
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    auto end = line.find_last_not_of(" \t\r\n");
    std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.rfind("--", 0) == 0)
      continue;
    current += trimmed + "\n";
    if (trimmed.back() == ';') {
      statements.push_back(current);
      current.clear();
    }
  }
  if (!current.empty())
    statements.push_back(current);
  return statements;
}

IdentityResult execStatement(const IdentityExecInput& input)
{
  IdentityResult result;
  char* rawError = nullptr;
  const int rc =
      sqlite3_exec(input.db, input.sql.c_str(), nullptr, nullptr, &rawError);
  if (rc != SQLITE_OK) {
    result.error = rawError ? rawError : sqlite3_errmsg(input.db);
    sqlite3_free(rawError);
    return result;
  }
  result.ok = true;
  return result;
}

IdentityHandleResult openHandle(const std::string& path, int flags)
{
  IdentityHandleResult result;
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(path.c_str(), &raw, flags, nullptr) != SQLITE_OK) {
    DbHandle failed(raw, sqlite3_close_v2);
    result.error = failed ? sqlite3_errmsg(failed.get())
                          : "sqlite3_open_v2 failed for " + path;
    return result;
  }
  result.db.reset(raw);
  result.ok = true;
  return result;
}

void hashByte(uint64_t& hash, uint8_t value)
{
  hash ^= value;
  hash *= kFnvPrime;
}

void hashBytes(uint64_t& hash, const void* data, size_t size)
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i)
    hashByte(hash, bytes[i]);
}

std::string hexHash(uint64_t hash)
{
  static const char* digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[hash & 0xF];
    hash >>= 4;
  }
  return out;
}

IdentityChecksumResult tableChecksum(const IdentityChecksumInput& input)
{
  IdentityChecksumResult result;
  const std::string sql = "SELECT * FROM \"" + input.schema + "\".\""
                          + input.table + "\" ORDER BY id";
  SqliteStmt stmt;
  if (!stmt.prepare(input.db, sql.c_str())) {
    result.error = sqlite3_errmsg(input.db);
    return result;
  }

  uint64_t hash = kFnvOffset;
  const int columns = sqlite3_column_count(stmt.get());
  int step = 0;
  while ((step = stmt.step()) == SQLITE_ROW) {
    ++result.rows;
    hashByte(hash, 0x00);
    for (int i = 0; i < columns; ++i) {
      const int type = sqlite3_column_type(stmt.get(), i);
      hashByte(hash, static_cast<uint8_t>(type));
      switch (type) {
        case SQLITE_INTEGER: {
          const int64_t value = sqlite3_column_int64(stmt.get(), i);
          hashBytes(hash, &value, sizeof(value));
          break;
        }
        case SQLITE_FLOAT: {
          const double value = sqlite3_column_double(stmt.get(), i);
          hashBytes(hash, &value, sizeof(value));
          break;
        }
        case SQLITE_TEXT:
        case SQLITE_BLOB: {
          const void* data = sqlite3_column_blob(stmt.get(), i);
          const int size = sqlite3_column_bytes(stmt.get(), i);
          hashBytes(hash, data, static_cast<size_t>(size));
          break;
        }
        default:
          break;
      }
    }
  }
  if (step != SQLITE_DONE) {
    result.error = sqlite3_errmsg(input.db);
    return result;
  }
  result.checksum = hexHash(hash);
  result.ok = true;
  return result;
}

std::optional<std::vector<std::string>>
columnNames(const IdentityColumnShapeInput& input, std::string& error)
{
  const std::string sql = "SELECT name FROM \"" + input.schema
                          + "\".pragma_table_info('" + input.table
                          + "') ORDER BY cid";
  SqliteStmt stmt;
  if (!stmt.prepare(input.db, sql.c_str())) {
    error = sqlite3_errmsg(input.db);
    return std::nullopt;
  }
  std::vector<std::string> names;
  while (stmt.step() == SQLITE_ROW)
    names.push_back(stmt.columnText(0));
  if (names.empty()) {
    error = "table " + input.schema + "." + input.table + " is missing";
    return std::nullopt;
  }
  return names;
}

bool copyIdentityTables(sqlite3* db, std::string& error)
{
  const auto begin = execStatement({.db = db, .sql = "BEGIN IMMEDIATE"});
  if (!begin.ok) {
    error = "cannot begin transaction: " + begin.error;
    return false;
  }
  for (const auto& table : kIdentityTables) {
    const std::string sql = "INSERT INTO main.\"" + table
                            + "\" SELECT * FROM src.\"" + table + "\"";
    const auto insert = execStatement({.db = db, .sql = sql});
    if (!insert.ok) {
      error = "copy of " + table + " failed: " + insert.error;
      execStatement({.db = db, .sql = "ROLLBACK"});
      return false;
    }
  }
  const auto commit = execStatement({.db = db, .sql = "COMMIT"});
  if (!commit.ok) {
    error = "cannot commit transaction: " + commit.error;
    execStatement({.db = db, .sql = "ROLLBACK"});
    return false;
  }
  return true;
}

IdentityResult validateDistinctPaths(const std::string& sourcePath,
                                     const std::string& targetPath)
{
  IdentityResult result;
  std::error_code ec;
  auto source = std::filesystem::weakly_canonical(sourcePath, ec);
  if (ec)
    source = std::filesystem::path(sourcePath).lexically_normal();
  auto target = std::filesystem::weakly_canonical(targetPath, ec);
  if (ec)
    target = std::filesystem::path(targetPath).lexically_normal();
  if (source == target) {
    result.error = "source and target are the same file: " + sourcePath;
    return result;
  }
  result.ok = true;
  return result;
}

IdentityResult validateSource(const std::string& sourcePath)
{
  IdentityResult result;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(sourcePath, ec)) {
    result.error = "source database not found: " + sourcePath;
    return result;
  }
  const auto source = openHandle(sourcePath, SQLITE_OPEN_READONLY);
  if (!source.ok) {
    result.error = "cannot open source read-only: " + source.error;
    return result;
  }
  for (const auto& table : kIdentityTables) {
    SqliteStmt stmt;
    const std::string sql = "SELECT 1 FROM sqlite_master WHERE type = 'table' "
                            "AND name = '" + table + "'";
    if (!stmt.prepare(source.db.get(), sql.c_str())) {
      result.error = sqlite3_errmsg(source.db.get());
      return result;
    }
    if (stmt.step() != SQLITE_ROW) {
      result.error = "source database is missing identity table: " + table;
      return result;
    }
  }
  result.ok = true;
  return result;
}

} // namespace

IdentityResult applyIdentitySchema(const IdentitySchemaInput& input)
{
  IdentityResult result;
  if (!input.db) {
    result.error = "no database handle";
    return result;
  }
  std::ifstream file(input.schemaPath);
  if (!file.is_open()) {
    result.error = "cannot open schema file: " + input.schemaPath;
    return result;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  for (const auto& statement : splitStatements(buffer.str())) {
    const auto executed = execStatement({.db = input.db, .sql = statement});
    if (!executed.ok) {
      result.error = executed.error + " in: "
                     + statement.substr(0, 80);
      return result;
    }
  }
  result.ok = true;
  return result;
}

IdentityMigrationReport verifyIdentityTables(const IdentityVerificationInput& input)
{
  IdentityMigrationReport report;
  if (!input.target) {
    report.error = "no target handle";
    return report;
  }

  for (const auto& table : kIdentityTables) {
    IdentityTableReport entry{.table = table,
                              .sourceRows = 0,
                              .targetRows = 0,
                              .sourceChecksum = std::string(),
                              .targetChecksum = std::string()};

    std::string shapeError;
    const auto sourceColumns =
        columnNames({.db = input.target, .schema = "src", .table = table},
                    shapeError);
    std::string targetColumnsError;
    const auto targetColumns =
        columnNames({.db = input.target, .schema = "main", .table = table},
                    targetColumnsError);
    if (!sourceColumns || !targetColumns) {
      report.error = shapeError.empty() ? targetColumnsError : shapeError;
      return report;
    }
    if (*sourceColumns != *targetColumns) {
      report.error = "column shape mismatch on " + table + ": source and "
                     "target schemas differ";
      return report;
    }
    const auto sourceChecksum =
        tableChecksum({.db = input.target, .schema = "src", .table = table});
    const auto targetChecksum =
        tableChecksum({.db = input.target, .schema = "main", .table = table});
    if (!sourceChecksum.ok || !targetChecksum.ok) {
      report.error = "checksum of " + table + " failed: "
                     + (sourceChecksum.ok ? targetChecksum.error
                                          : sourceChecksum.error);
      return report;
    }
    entry.sourceRows = sourceChecksum.rows;
    entry.sourceChecksum = sourceChecksum.checksum;
    entry.targetRows = targetChecksum.rows;
    entry.targetChecksum = targetChecksum.checksum;

    if (entry.sourceRows != entry.targetRows
        || entry.sourceChecksum != entry.targetChecksum) {
      report.error = "mismatch on " + table + ": rows " + std::to_string(entry.sourceRows)
                     + " vs " + std::to_string(entry.targetRows)
                     + ", checksum " + entry.sourceChecksum + " vs "
                     + entry.targetChecksum;
      report.tables.push_back(entry);
      return report;
    }
    report.tables.push_back(std::move(entry));
  }

  report.ok = true;
  return report;
}

IdentityMigrationReport migrateIdentity(const IdentityMigrationOptions& options)
{
  IdentityMigrationReport report;
  if (options.sourcePath.empty() || options.targetPath.empty()
      || options.schemaPath.empty()) {
    report.error = "source, target and schema paths are required";
    return report;
  }

  const auto distinct =
      validateDistinctPaths(options.sourcePath, options.targetPath);
  if (!distinct.ok) {
    report.error = distinct.error;
    return report;
  }

  const auto sourceCheck = validateSource(options.sourcePath);
  if (!sourceCheck.ok) {
    report.error = sourceCheck.error;
    return report;
  }

  std::error_code schemaEc;
  if (!std::filesystem::is_regular_file(options.schemaPath, schemaEc)) {
    report.error = "schema file not found: " + options.schemaPath;
    return report;
  }

  for (const auto* suffix : {"", "-wal", "-shm"})
    std::remove((options.targetPath + suffix).c_str());

  auto target = openHandle(
      options.targetPath,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
  if (!target.ok) {
    report.error = "cannot open target: " + target.error;
    return report;
  }

  const auto busy =
      execStatement({.db = target.db.get(), .sql = "PRAGMA busy_timeout = 5000"});
  if (!busy.ok) {
    report.error = "cannot set busy_timeout: " + busy.error;
    return report;
  }

  const auto schema = applyIdentitySchema(
      {.db = target.db.get(), .schemaPath = options.schemaPath});
  if (!schema.ok) {
    report.error = "schema application failed: " + schema.error;
    return report;
  }

  std::string sourcePath = options.sourcePath;
  for (size_t at = sourcePath.find('\''); at != std::string::npos;
       at = sourcePath.find('\'', at + 2))
    sourcePath.replace(at, 1, "''");
  const auto uri = "file:" + sourcePath + "?mode=ro";
  const auto attach =
      execStatement({.db = target.db.get(),
                     .sql = "ATTACH DATABASE '" + uri + "' AS src"});
  if (!attach.ok) {
    report.error = "cannot attach source read-only: " + attach.error;
    return report;
  }

  std::string copyError;
  if (!copyIdentityTables(target.db.get(), copyError)) {
    report.error = copyError;
    execStatement({.db = target.db.get(), .sql = "DETACH DATABASE src"});
    return report;
  }

  report = verifyIdentityTables({.target = target.db.get()});

  execStatement({.db = target.db.get(), .sql = "DETACH DATABASE src"});
  return report;
}
