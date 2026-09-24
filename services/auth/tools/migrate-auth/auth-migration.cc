#include "auth-migration.hxx"

#include <sqlite/sql-escape.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <text/fnv-hash.hxx>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

namespace
{

constexpr std::array<std::string_view, 3> kAuthTables{
    "refresh_token",
    "device_login_challenge",
    "device_credential"};

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct AuthHandleResult
{
  bool ok = false;
  DbHandle db{nullptr, sqlite3_close_v2};
  std::string error;
};

struct AuthExecInput
{
  sqlite3* db = nullptr;
  std::string sql;
};

struct AuthChecksumInput
{
  sqlite3* db = nullptr;
  std::string table;
  std::string schema;
};

struct AuthChecksumResult
{
  bool ok = false;
  int64_t rows = 0;
  std::string checksum;
  std::string error;
};

struct AuthColumnShapeInput
{
  sqlite3* db = nullptr;
  std::string schema;
  std::string table;
};

struct AuthDistinctPathsInput
{
  std::string source;
  std::string target;
};

std::vector<std::string> splitStatements(const std::string& script)
{
  std::vector<std::string> statements;
  std::string current;
  std::istringstream ss(script);
  std::string line;
  while (std::getline(ss, line)) {
    const auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    const auto end = line.find_last_not_of(" \t\r\n");
    const std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.starts_with("--"))
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

AuthResult execStatement(const AuthExecInput& input)
{
  AuthResult result;
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

AuthHandleResult openHandle(const std::string& path, int flags)
{
  AuthHandleResult result;
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

std::string scopedSelect(const AuthChecksumInput& input)
{
  const std::string quoted = "\"" + input.table + "\"";
  if (input.schema == "src")
    return "SELECT * FROM src." + quoted + " ORDER BY id";
  return "SELECT * FROM main." + quoted + " WHERE id IN (SELECT id FROM src."
         + quoted + ") ORDER BY id";
}

AuthChecksumResult tableChecksum(const AuthChecksumInput& input)
{
  AuthChecksumResult result;
  SqliteStmt stmt;
  const std::string sql = scopedSelect(input);
  if (!stmt.prepare(input.db, sql.c_str())) {
    result.error = sqlite3_errmsg(input.db);
    return result;
  }

  Fnv1a hash;
  const int columns = sqlite3_column_count(stmt.get());
  int step = 0;
  while ((step = stmt.step()) == SQLITE_ROW) {
    ++result.rows;
    hash.add(0x00);
    for (int i = 0; i < columns; ++i) {
      const int type = sqlite3_column_type(stmt.get(), i);
      hash.add(static_cast<uint8_t>(type));
      switch (type) {
        case SQLITE_INTEGER: {
          const int64_t value = sqlite3_column_int64(stmt.get(), i);
          hash.add(&value, sizeof(value));
          break;
        }
        case SQLITE_FLOAT: {
          const double value = sqlite3_column_double(stmt.get(), i);
          hash.add(&value, sizeof(value));
          break;
        }
        case SQLITE_TEXT:
        case SQLITE_BLOB: {
          const void* data = sqlite3_column_blob(stmt.get(), i);
          const int size = sqlite3_column_bytes(stmt.get(), i);
          hash.add(data, static_cast<size_t>(size));
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
  result.checksum = hash.hex();
  result.ok = true;
  return result;
}

std::optional<std::vector<std::string>>
columnNames(const AuthColumnShapeInput& input, std::string& error)
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

bool copyAuthTables(sqlite3* db, std::string& error)
{
  const auto begin = execStatement({.db = db, .sql = "BEGIN IMMEDIATE"});
  if (!begin.ok) {
    error = "cannot begin transaction: " + begin.error;
    return false;
  }
  for (const std::string_view table : kAuthTables) {
    std::string sql = "INSERT INTO main.\"";
    sql += table;
    sql += "\" SELECT * FROM src.\"";
    sql += table;
    sql += "\" AS s WHERE NOT EXISTS (SELECT 1 FROM main.\"";
    sql += table;
    sql += "\" AS m WHERE m.id = s.id)";
    const auto insert = execStatement({.db = db, .sql = std::move(sql)});
    if (!insert.ok) {
      error = "copy of " + std::string(table) + " failed: " + insert.error;
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

AuthResult validateDistinctPaths(const AuthDistinctPathsInput& input)
{
  AuthResult result;
  std::error_code ec;
  auto source = std::filesystem::weakly_canonical(input.source, ec);
  if (ec)
    source = std::filesystem::path(input.source).lexically_normal();
  auto target = std::filesystem::weakly_canonical(input.target, ec);
  if (ec)
    target = std::filesystem::path(input.target).lexically_normal();
  if (source == target) {
    result.error = "source and target are the same file: " + input.source;
    return result;
  }
  result.ok = true;
  return result;
}

AuthResult validateSource(const std::string& sourcePath)
{
  AuthResult result;
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
  for (const std::string_view table : kAuthTables) {
    SqliteStmt stmt;
    const std::string sql =
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '"
        + std::string(table) + "'";
    if (!stmt.prepare(source.db.get(), sql.c_str())) {
      result.error = sqlite3_errmsg(source.db.get());
      return result;
    }
    if (stmt.step() != SQLITE_ROW) {
      result.error = "source database is missing auth table: "
                     + std::string(table);
      return result;
    }
  }
  result.ok = true;
  return result;
}

}

AuthResult applyAuthSchema(const AuthSchemaInput& input)
{
  AuthResult result;
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
      result.error = executed.error + " in: " + statement.substr(0, 80);
      return result;
    }
  }
  result.ok = true;
  return result;
}

AuthMigrationReport verifyAuthTables(const AuthVerificationInput& input)
{
  AuthMigrationReport report;
  if (!input.target) {
    report.error = "no target handle";
    return report;
  }

  for (const std::string_view table : kAuthTables) {
    const std::string tableName(table);
    AuthTableReport entry{.table = tableName,
                          .sourceRows = 0,
                          .targetRows = 0,
                          .sourceChecksum = std::string(),
                          .targetChecksum = std::string()};

    std::string shapeError;
    const auto sourceColumns =
        columnNames({.db = input.target, .schema = "src", .table = tableName},
                    shapeError);
    std::string targetColumnsError;
    const auto targetColumns =
        columnNames({.db = input.target, .schema = "main", .table = tableName},
                    targetColumnsError);
    if (!sourceColumns || !targetColumns) {
      report.error = shapeError.empty() ? targetColumnsError : shapeError;
      return report;
    }
    if (*sourceColumns != *targetColumns) {
      report.error = "column shape mismatch on " + tableName
                     + ": source and target schemas differ";
      return report;
    }

    const auto sourceChecksum = tableChecksum(
        {.db = input.target, .table = tableName, .schema = "src"});
    const auto targetChecksum = tableChecksum(
        {.db = input.target, .table = tableName, .schema = "main"});
    if (!sourceChecksum.ok || !targetChecksum.ok) {
      report.error = "checksum of " + tableName + " failed: "
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
      report.error = "mismatch on " + tableName + ": rows "
                     + std::to_string(entry.sourceRows) + " vs "
                     + std::to_string(entry.targetRows) + ", checksum "
                     + entry.sourceChecksum + " vs " + entry.targetChecksum;
      report.tables.push_back(entry);
      return report;
    }
    report.tables.push_back(std::move(entry));
  }

  report.ok = true;
  return report;
}

AuthMigrationReport migrateAuth(const AuthMigrationOptions& options)
{
  AuthMigrationReport report;
  if (options.sourcePath.empty() || options.targetPath.empty()
      || options.schemaPath.empty()) {
    report.error = "source, target and schema paths are required";
    return report;
  }

  const auto distinct = validateDistinctPaths(
      {.source = options.sourcePath, .target = options.targetPath});
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

  auto target = openHandle(options.targetPath,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                               | SQLITE_OPEN_URI);
  if (!target.ok) {
    report.error = "cannot open target: " + target.error;
    return report;
  }

  const auto busy = execStatement(
      {.db = target.db.get(), .sql = "PRAGMA busy_timeout = 5000"});
  if (!busy.ok) {
    report.error = "cannot set busy_timeout: " + busy.error;
    return report;
  }

  const auto schema = applyAuthSchema(
      {.db = target.db.get(), .schemaPath = options.schemaPath});
  if (!schema.ok) {
    report.error = "schema application failed: " + schema.error;
    return report;
  }

  const std::string sourcePath = sql_util::escapeLiteral(options.sourcePath);
  const auto attach = execStatement(
      {.db = target.db.get(),
       .sql = "ATTACH DATABASE 'file:" + sourcePath + "?mode=ro' AS src"});
  if (!attach.ok) {
    report.error = "cannot attach source read-only: " + attach.error;
    return report;
  }

  std::string copyError;
  if (!copyAuthTables(target.db.get(), copyError)) {
    report.error = copyError;
    execStatement({.db = target.db.get(), .sql = "DETACH DATABASE src"});
    return report;
  }

  report = verifyAuthTables({.target = target.db.get()});

  execStatement({.db = target.db.get(), .sql = "DETACH DATABASE src"});
  return report;
}
