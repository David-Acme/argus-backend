#include "table-rebuild.hxx"

#include "sqlite-stmt.hxx"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{
using Db = sqlite3*;

struct Pending
{
  std::string table;
  std::string rebuiltDefinition;
};

std::string quoted(std::string_view name)
{
  std::string out = "\"";
  for (const char c : name) {
    if (c == '"')
      out += '"';
    out += c;
  }
  out += '"';
  return out;
}

std::string lastError(Db db, std::string_view during)
{
  return std::string(during) + ": " + sqlite3_errmsg(db);
}

bool exec(Db db, const std::string& sql, std::string& error)
{
  char* message = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK)
    return true;
  error = std::string(message != nullptr ? message : "sqlite3_exec failed") + " [" + sql.substr(0, 80) + "]";
  sqlite3_free(message);
  return false;
}

std::optional<std::string> textOf(Db db, const std::string& sql, const std::string& argument)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()) || !stmt.bindText(1, argument))
    return std::nullopt;
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
  return text == nullptr ? std::optional<std::string>(std::string()) : std::optional<std::string>(text);
}

std::optional<std::int64_t> integerOf(Db db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()) || stmt.step() != SQLITE_ROW)
    return std::nullopt;
  return sqlite3_column_int64(stmt.get(), 0);
}

std::vector<std::string> columnsOf(Db db, const std::string& table)
{
  std::vector<std::string> columns;
  SqliteStmt stmt;
  if (!stmt.prepare(db, "SELECT name FROM pragma_table_info(?) ORDER BY cid") || !stmt.bindText(1, table))
    return columns;
  while (stmt.step() == SQLITE_ROW)
    columns.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
  return columns;
}

std::vector<std::string> dependentsOf(Db db, const std::string& table)
{
  std::vector<std::string> statements;
  SqliteStmt stmt;
  if (!stmt.prepare(db,
                    "SELECT sql FROM sqlite_master WHERE tbl_name = ? AND type IN ('index', 'trigger') "
                    "AND sql IS NOT NULL ORDER BY type DESC, name") ||
      !stmt.bindText(1, table))
    return statements;
  while (stmt.step() == SQLITE_ROW)
    statements.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
  return statements;
}

bool viewMentions(Db db, const std::vector<std::string>& tables)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, "SELECT sql FROM sqlite_master WHERE type = 'view' AND sql IS NOT NULL"))
    return true;
  while (stmt.step() == SQLITE_ROW) {
    const std::string sql = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const bool mentions = std::ranges::any_of(tables, [&](const std::string& table) {
      return sql.find(table) != std::string::npos;
    });
    if (mentions)
      return true;
  }
  return false;
}

bool plainName(std::string_view name)
{
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}

std::string columnList(const std::vector<std::string>& columns)
{
  std::string list;
  for (const auto& column : columns) {
    if (!list.empty())
      list += ", ";
    list += quoted(column);
  }
  return list;
}

bool backUp(Db db, const std::string& path, std::string& error)
{
  Db copy = nullptr;
  if (sqlite3_open(path.c_str(), &copy) != SQLITE_OK) {
    error = "backup: could not create " + path;
    sqlite3_close(copy);
    return false;
  }
  sqlite3_backup* backup = sqlite3_backup_init(copy, "main", db, "main");
  if (backup == nullptr) {
    error = lastError(copy, "backup");
    sqlite3_close(copy);
    return false;
  }
  const int stepped = sqlite3_backup_step(backup, -1);
  sqlite3_backup_finish(backup);
  const bool done = stepped == SQLITE_DONE;
  if (!done)
    error = "backup: copy did not complete";
  sqlite3_close(copy);
  return done;
}

bool rebuildOne(Db db, const Pending& pending, std::string& error)
{
  const std::string temporary = pending.table + "__rebuild";
  const auto columns = columnsOf(db, pending.table);
  if (columns.empty()) {
    error = pending.table + ": no columns";
    return false;
  }
  const auto before = integerOf(db, "SELECT COUNT(*) FROM " + quoted(pending.table));
  const auto dependents = dependentsOf(db, pending.table);
  const auto sequence = textOf(db, "SELECT CAST(seq AS TEXT) FROM sqlite_sequence WHERE name = ?", pending.table);

  const auto open = pending.rebuiltDefinition.find('(');
  if (open == std::string::npos) {
    error = pending.table + ": the definition has no column list";
    return false;
  }
  const std::string list = columnList(columns);
  if (!exec(db, "CREATE TABLE " + quoted(temporary) + " " + pending.rebuiltDefinition.substr(open), error) ||
      !exec(db,
            "INSERT INTO " + quoted(temporary) + " (" + list + ") SELECT " + list + " FROM " + quoted(pending.table),
            error))
    return false;
  const auto copied = integerOf(db, "SELECT COUNT(*) FROM " + quoted(temporary));
  if (!before || !copied || *before != *copied) {
    error = pending.table + ": the copy holds a different number of rows";
    return false;
  }
  if (!exec(db, "DROP TABLE " + quoted(pending.table), error) ||
      !exec(db, "ALTER TABLE " + quoted(temporary) + " RENAME TO " + quoted(pending.table), error))
    return false;
  for (const auto& statement : dependents)
    if (!exec(db, statement, error))
      return false;
  if (sequence && !sequence->empty()) {
    const std::string kept = *sequence;
    if (!exec(db,
              "UPDATE sqlite_sequence SET seq = MAX(seq, " + kept + ") WHERE name = '" + pending.table + "'",
              error))
      return false;
    if (!integerOf(db, "SELECT 1 FROM sqlite_sequence WHERE name = '" + pending.table + "'") &&
        !exec(db, "INSERT INTO sqlite_sequence (name, seq) VALUES ('" + pending.table + "', " + kept + ")", error))
      return false;
  }
  return true;
}

void restoreForeignKeys(Db db, bool wasOn)
{
  if (wasOn) {
    char* message = nullptr;
    sqlite3_exec(db, "PRAGMA foreign_keys = ON", nullptr, nullptr, &message);
    sqlite3_free(message);
  }
}
}

namespace table_rebuild
{
Outcome run(const Input& input)
{
  Outcome outcome;
  Db db = input.db;
  std::vector<Pending> pending;
  for (const auto& table : input.tables) {
    const auto definition =
        textOf(db, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?", table);
    if (!definition || definition->empty())
      continue;
    if (auto rewritten = input.rewrite(*definition))
      pending.push_back({.table = table, .rebuiltDefinition = std::move(*rewritten)});
  }
  if (pending.empty())
    return outcome;
  std::vector<std::string> names;
  for (const auto& item : pending)
    names.push_back(item.table);
  if (!std::ranges::all_of(names, plainName)) {
    outcome.error = "a table name holds characters a rebuild does not carry";
    return outcome;
  }
  if (viewMentions(db, names)) {
    outcome.error = "a view depends on a table to rebuild; it is not carried over";
    return outcome;
  }

  if (!input.backupPath.empty()) {
    if (!backUp(db, input.backupPath, outcome.error))
      return outcome;
    outcome.backup = input.backupPath;
  }

  const bool foreignKeysWereOn = integerOf(db, "PRAGMA foreign_keys").value_or(0) != 0;
  if (!exec(db, "PRAGMA foreign_keys = OFF", outcome.error))
    return outcome;
  if (integerOf(db, "PRAGMA foreign_keys").value_or(1) != 0) {
    outcome.error = "foreign keys could not be switched off outside a transaction";
    restoreForeignKeys(db, foreignKeysWereOn);
    return outcome;
  }

  std::string error;
  bool done = exec(db, "BEGIN IMMEDIATE", error);
  if (done) {
    for (const auto& item : pending) {
      done = rebuildOne(db, item, error);
      if (!done)
        break;
    }
    if (done) {
      SqliteStmt check;
      if (!check.prepare(db, "PRAGMA foreign_key_check")) {
        error = lastError(db, "foreign_key_check");
        done = false;
      }
      else if (check.step() == SQLITE_ROW) {
        error = "foreign_key_check reports a broken reference after the rebuild";
        done = false;
      }
    }
    if (done)
      done = exec(db, "COMMIT", error);
    else {
      std::string ignored;
      exec(db, "ROLLBACK", ignored);
    }
  }
  restoreForeignKeys(db, foreignKeysWereOn);
  if (!done) {
    outcome.error = error;
    return outcome;
  }
  for (const auto& item : pending)
    outcome.rebuilt.push_back(item.table);
  return outcome;
}
}
