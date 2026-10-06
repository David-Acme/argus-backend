#include "sync-migration.hxx"

#include <sqlite/sql-escape.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <text/fnv-hash.hxx>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

struct SyncTableSpec
{
  std::string_view table;
  std::string_view key;
};

constexpr std::array<SyncTableSpec, 5> kSyncTables = {{
    {.table = "audit_log", .key = "id"},
    {.table = "user_audit_log", .key = "id"},
    {.table = "audit_compaction_state", .key = "table_name"},
    {.table = "user_action_log", .key = "id"},
    {.table = "notification_delivery_inbox", .key = "delivery_id"},
}};

constexpr std::string_view kSourceSchema = "src";
constexpr std::string_view kTargetSchema = "main";
constexpr std::string_view kPathUnreserved =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~/";
constexpr std::string_view kUpperHex = "0123456789ABCDEF";

struct AdditiveColumn
{
  std::string_view table;
  std::string_view column;
};

constexpr std::array<AdditiveColumn, 1> kAdditiveColumns = {{
    {.table = "user_action_log", .column = "module"},
}};

struct SyncPathPair
{
  std::string sourcePath;
  std::string targetPath;
};

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct SyncHandleResult
{
  bool ok = false;
  DbHandle db{nullptr, sqlite3_close_v2};
  std::string error;
};

struct SyncExecInput
{
  sqlite3* db = nullptr;
  std::string sql;
};

struct SyncTableQueryInput
{
  sqlite3* db = nullptr;
  std::string schema;
  std::string table;
};

struct SyncColumnResult
{
  bool ok = false;
  std::vector<std::string> names;
  std::string error;
};

struct SyncAttachResult
{
  bool ok = false;
  bool readWrite = false;
  std::string error;
};

struct SyncTablePlan
{
  std::size_t index = 0;
  std::string table;
  std::string key;
  std::vector<std::string> columns;
};

struct SyncChecksumInput
{
  sqlite3* db = nullptr;
  std::string sql;
};

struct SyncChecksumResult
{
  bool ok = false;
  int64_t rows = 0;
  std::string checksum;
  std::string error;
};

struct SyncCountResult
{
  bool ok = false;
  int64_t rows = 0;
  std::string error;
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

SyncResult execStatement(const SyncExecInput& input)
{
  SyncResult result;
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

SyncHandleResult openHandle(const std::string& path, int flags)
{
  SyncHandleResult result;
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(path.c_str(), &raw, flags, nullptr) != SQLITE_OK) {
    const DbHandle failed(raw, sqlite3_close_v2);
    result.error = failed ? sqlite3_errmsg(failed.get())
                          : "sqlite3_open_v2 failed for " + path;
    return result;
  }
  result.db.reset(raw);
  result.ok = true;
  return result;
}

std::optional<bool> tableExists(const SyncTableQueryInput& input)
{
  const std::string sql = "SELECT 1 FROM \"" + input.schema
                          + "\".sqlite_master WHERE type = 'table' AND name = '"
                          + input.table + "'";
  SqliteStmt stmt;
  if (!stmt.prepare(input.db, sql.c_str()))
    return std::nullopt;
  return stmt.step() == SQLITE_ROW;
}

SyncColumnResult columnNames(const SyncTableQueryInput& input)
{
  SqliteStmt stmt;
  if (!stmt.prepare(
          input.db,
          "SELECT name FROM pragma_table_info(?, ?) ORDER BY cid")) {
    SyncColumnResult result;
    result.error = sqlite3_errmsg(input.db);
    return result;
  }
  stmt.bindText(1, input.table);
  stmt.bindText(2, input.schema);

  SyncColumnResult result;
  while (stmt.step() == SQLITE_ROW)
    result.names.push_back(stmt.columnText(0));
  if (result.names.empty()) {
    result.error =
        "table " + input.schema + "." + input.table + " is missing";
    return result;
  }
  result.ok = true;
  return result;
}

std::string joinColumns(const std::vector<std::string>& columns)
{
  std::string joined;
  for (const auto& column : columns) {
    if (!joined.empty())
      joined += ", ";
    joined += column;
  }
  return joined;
}

struct ExpectedColumnsInput
{
  std::string table;
  std::vector<std::string> source;
  std::vector<std::string> target;
};

std::vector<std::string> expectedColumns(ExpectedColumnsInput input)
{
  for (const auto& additive : kAdditiveColumns) {
    if (additive.table != input.table)
      continue;
    const std::string column(additive.column);
    if (std::ranges::find(input.source, column) == input.source.end())
      std::erase(input.target, column);
  }
  return input.target;
}

bool sameColumnSet(std::vector<std::string> source,
                   std::vector<std::string> target)
{
  std::ranges::sort(source);
  std::ranges::sort(target);
  return source == target;
}

std::string copiedKeyTable(const std::string& table)
{
  return "sync_copied_" + table;
}

std::string copiedKeyTableSql(const SyncTablePlan& plan)
{
  return "CREATE TEMP TABLE \"" + copiedKeyTable(plan.table)
         + "\" AS SELECT s.\"" + plan.key + "\" AS key_value FROM src.\""
         + plan.table + "\" AS s WHERE NOT EXISTS (SELECT 1 FROM main.\""
         + plan.table + "\" AS m WHERE m.\"" + plan.key + "\" = s.\""
         + plan.key + "\")";
}

std::string copySql(const SyncTablePlan& plan)
{
  std::string values;
  for (const auto& column : plan.columns) {
    if (!values.empty())
      values += ", ";
    values += "s.\"" + column + "\"";
  }
  return "INSERT INTO main.\"" + plan.table + "\" ("
         + joinColumns(plan.columns) + ") SELECT " + values + " FROM src.\""
         + plan.table + "\" AS s WHERE NOT EXISTS (SELECT 1 FROM main.\""
         + plan.table + "\" AS m WHERE m.\"" + plan.key + "\" = s.\""
         + plan.key + "\")";
}

std::string scopedSelectSql(const std::string_view schema,
                            const SyncTablePlan& plan)
{
  std::string columns;
  for (const auto& column : plan.columns) {
    if (!columns.empty())
      columns += ", ";
    columns += "t.\"" + column + "\"";
  }
  return "SELECT " + columns + " FROM \"" + std::string(schema) + "\".\""
         + plan.table + "\" AS t JOIN temp.\"" + copiedKeyTable(plan.table)
         + "\" AS c ON c.key_value = t.\"" + plan.key + "\" ORDER BY t.\""
         + plan.key + "\"";
}

std::string sourceCountSql(const SyncTablePlan& plan)
{
  return "SELECT COUNT(*) AS total FROM src.\"" + plan.table + "\"";
}

SyncChecksumResult tableChecksum(const SyncChecksumInput& input)
{
  SyncChecksumResult result;
  SqliteStmt stmt;
  if (!stmt.prepare(input.db, input.sql.c_str())) {
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
      const char* name = sqlite3_column_name(stmt.get(), i);
      if (name != nullptr)
        hash.add(name, std::strlen(name));
      hash.add(0x1F);
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

SyncCountResult countRows(const SyncChecksumInput& input)
{
  SyncCountResult result;
  SqliteStmt stmt;
  if (!stmt.prepare(input.db, input.sql.c_str())) {
    result.error = sqlite3_errmsg(input.db);
    return result;
  }
  if (stmt.step() != SQLITE_ROW) {
    result.error = sqlite3_errmsg(input.db);
    return result;
  }
  result.rows = stmt.columnInt64(0);
  result.ok = true;
  return result;
}

std::string attachedSourceUri(const std::string& path)
{
  std::string uri("file:");
  uri.reserve(path.size() + 16);
  for (const unsigned char byte : path) {
    const auto character = static_cast<char>(byte);
    if (kPathUnreserved.find(character) != std::string_view::npos) {
      uri.push_back(character);
      continue;
    }
    uri.push_back('%');
    uri.push_back(kUpperHex[byte >> 4U]);
    uri.push_back(kUpperHex[byte & 0x0FU]);
  }
  uri += "?mode=ro";
  return uri;
}

SyncResult buildPlans(sqlite3* db,
                      std::vector<SyncTablePlan>& plans,
                      std::vector<SyncTableReport>& entries)
{
  entries.resize(kSyncTables.size());
  for (std::size_t index = 0; index < kSyncTables.size(); ++index) {
    const auto& spec = kSyncTables[index];
    entries[index].table = std::string(spec.table);

    const std::string table(spec.table);
    const auto exists = tableExists({.db = db,
                                     .schema = std::string(kSourceSchema),
                                     .table = table});
    if (!exists.has_value())
      return {.ok = false,
              .error = "cannot read the source schema: "
                       + std::string(sqlite3_errmsg(db))};
    if (!*exists)
      continue;
    entries[index].present = true;

    const auto sourceColumns = columnNames({.db = db,
                                            .schema = std::string(kSourceSchema),
                                            .table = table});
    if (!sourceColumns.ok)
      return {.ok = false, .error = sourceColumns.error};
    const auto targetColumns = columnNames({.db = db,
                                            .schema = std::string(kTargetSchema),
                                            .table = table});
    if (!targetColumns.ok)
      return {.ok = false, .error = targetColumns.error};
    auto expected = expectedColumns({.table = table, .source = sourceColumns.names, .target = targetColumns.names});
    if (!sameColumnSet(sourceColumns.names, expected)) {
      return {.ok = false,
              .error = "column shape mismatch on " + table + ": source ("
                       + joinColumns(sourceColumns.names) + ") vs target ("
                       + joinColumns(targetColumns.names) + ")"};
    }

    plans.push_back({.index = index,
                     .table = table,
                     .key = std::string(spec.key),
                     .columns = std::move(expected)});
  }
  return {.ok = true, .error = ""};
}

SyncResult copyTables(sqlite3* db, const std::vector<SyncTablePlan>& plans)
{
  for (const auto& plan : plans) {
    for (const auto& sql : {copiedKeyTableSql(plan), copySql(plan)}) {
      const auto executed = execStatement({.db = db, .sql = sql});
      if (!executed.ok)
        return {.ok = false,
                .error = "copy of " + plan.table + " failed: " + executed.error};
    }
  }
  return {.ok = true, .error = ""};
}

SyncResult verifyTables(sqlite3* db,
                        const std::vector<SyncTablePlan>& plans,
                        std::vector<SyncTableReport>& entries)
{
  for (const auto& plan : plans) {
    const auto sourceRows =
        countRows({.db = db, .sql = sourceCountSql(plan)});
    if (!sourceRows.ok)
      return {.ok = false,
              .error = "counting " + plan.table + " failed: "
                       + sourceRows.error};

    const auto sourceChecksum = tableChecksum(
        {.db = db, .sql = scopedSelectSql(kSourceSchema, plan)});
    const auto targetChecksum = tableChecksum(
        {.db = db, .sql = scopedSelectSql(kTargetSchema, plan)});
    if (!sourceChecksum.ok || !targetChecksum.ok) {
      return {.ok = false,
              .error = "checksum of " + plan.table + " failed: "
                       + (sourceChecksum.ok ? targetChecksum.error
                                            : sourceChecksum.error)};
    }
    if (sourceChecksum.rows != targetChecksum.rows
        || sourceChecksum.checksum != targetChecksum.checksum) {
      return {.ok = false,
              .error = "mismatch on " + plan.table + ": rows "
                       + std::to_string(sourceChecksum.rows) + " vs "
                       + std::to_string(targetChecksum.rows) + ", checksum "
                       + sourceChecksum.checksum + " vs "
                       + targetChecksum.checksum};
    }

    auto& entry = entries[plan.index];
    entry.copiedRows = sourceChecksum.rows;
    entry.skippedRows = sourceRows.rows - sourceChecksum.rows;
    entry.sourceChecksum = sourceChecksum.checksum;
    entry.targetChecksum = targetChecksum.checksum;
  }
  return {.ok = true, .error = ""};
}

SyncResult validateDistinctPaths(const SyncPathPair& paths)
{
  SyncResult result;
  std::error_code ec;
  auto source = std::filesystem::weakly_canonical(paths.sourcePath, ec);
  if (ec)
    source = std::filesystem::path(paths.sourcePath).lexically_normal();
  auto target = std::filesystem::weakly_canonical(paths.targetPath, ec);
  if (ec)
    target = std::filesystem::path(paths.targetPath).lexically_normal();
  if (source == target) {
    result.error = "source and target are the same file: " + paths.sourcePath;
    return result;
  }
  if (std::filesystem::exists(paths.targetPath, ec)
      && std::filesystem::equivalent(paths.sourcePath, paths.targetPath, ec)) {
    result.error = "source and target are the same file: " + paths.sourcePath;
    return result;
  }
  result.ok = true;
  return result;
}

SyncResult validateSource(const std::string& sourcePath)
{
  SyncResult result;
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
  SqliteStmt stmt;
  if (!stmt.prepare(source.db.get(), "SELECT count(*) FROM sqlite_master")
      || stmt.step() != SQLITE_ROW) {
    result.error = "source database is unreadable: "
                   + std::string(sqlite3_errmsg(source.db.get()));
    return result;
  }
  result.ok = true;
  return result;
}

SyncAttachResult attachSource(sqlite3* db, const std::string& sourcePath)
{
  const std::string escaped = sql_util::escapeLiteral(
      attachedSourceUri(sourcePath));
  const auto readOnly = execStatement(
      {.db = db, .sql = "ATTACH DATABASE '" + escaped + "' AS src"});
  if (readOnly.ok)
    return {.ok = true, .readWrite = false, .error = ""};

  const std::string plain =
      sql_util::escapeLiteral(std::filesystem::path(sourcePath).string());
  const auto writable =
      execStatement({.db = db, .sql = "ATTACH DATABASE '" + plain + "' AS src"});
  if (writable.ok)
    return {.ok = true, .readWrite = true, .error = ""};

  return {.ok = false,
          .readWrite = false,
          .error = "cannot attach source: " + writable.error
                   + " (read-only attach: " + readOnly.error + ")"};
}

}

SyncResult applySyncSchema(const SyncSchemaInput& input)
{
  SyncResult result;
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

SyncMigrationReport migrateSync(const SyncMigrationOptions& options)
{
  SyncMigrationReport report;
  if (options.sourcePath.empty() || options.targetPath.empty()
      || options.schemaPath.empty()) {
    report.error = "source, target and schema paths are required";
    return report;
  }

  const auto distinct = validateDistinctPaths(
      {.sourcePath = options.sourcePath, .targetPath = options.targetPath});
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

  auto target = openHandle(
      options.targetPath,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
  if (!target.ok) {
    report.error = "cannot open target: " + target.error;
    return report;
  }
  sqlite3* db = target.db.get();

  const auto busy = execStatement({.db = db, .sql = "PRAGMA busy_timeout = 5000"});
  if (!busy.ok) {
    report.error = "cannot set busy_timeout: " + busy.error;
    return report;
  }

  const auto schema =
      applySyncSchema({.db = db, .schemaPath = options.schemaPath});
  if (!schema.ok) {
    report.error = "schema application failed: " + schema.error;
    return report;
  }

  const auto attach = attachSource(db, options.sourcePath);
  if (!attach.ok) {
    report.error = attach.error;
    return report;
  }
  report.sourceReadWrite = attach.readWrite;

  const auto detach = [&db] {
    execStatement({.db = db, .sql = "DETACH DATABASE src"});
  };

  std::vector<SyncTablePlan> plans;
  std::vector<SyncTableReport> entries;
  const auto planned = buildPlans(db, plans, entries);
  if (!planned.ok) {
    report.error = planned.error;
    detach();
    return report;
  }

  const auto begin = execStatement({.db = db, .sql = "BEGIN IMMEDIATE"});
  if (!begin.ok) {
    report.error = "cannot begin transaction: " + begin.error;
    detach();
    return report;
  }

  const auto rollback = [&](const std::string& error) {
    execStatement({.db = db, .sql = "ROLLBACK"});
    report.error = error;
    detach();
    return report;
  };

  const auto copied = copyTables(db, plans);
  if (!copied.ok)
    return rollback(copied.error);

  const auto verified = verifyTables(db, plans, entries);
  if (!verified.ok)
    return rollback(verified.error);

  const auto commit = execStatement({.db = db, .sql = "COMMIT"});
  if (!commit.ok)
    return rollback("cannot commit transaction: " + commit.error);

  report.tables = entries;
  report.ok = true;
  detach();
  return report;
}
