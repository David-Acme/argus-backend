#pragma once

#include <sqlite3.h>
#include <string>
#include <utility>

// RAII wrapper for sqlite3_stmt: prepared statements are always finalized,
// never leaked. Move-only, like the underlying resource. The raw sqlite3_*
// bind/column functions still work through get() when a convenience method
// is missing.
class SqliteStmt
{
public:
  SqliteStmt() = default;
  ~SqliteStmt() { finalize(); }

  SqliteStmt(const SqliteStmt&) = delete;
  SqliteStmt& operator=(const SqliteStmt&) = delete;

  SqliteStmt(SqliteStmt&& other) noexcept : stmt_(other.release()) {}
  SqliteStmt& operator=(SqliteStmt&& other) noexcept
  {
    if (this != &other) {
      finalize();
      stmt_ = other.release();
    }
    return *this;
  }

  // Prepares the statement on db; returns false on failure (same semantics
  // as sqlite3_prepare_v2). Any previously held statement is finalized.
  bool prepare(sqlite3* db, const char* sql)
  {
    finalize();
    return sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
  }

  int step() { return sqlite3_step(stmt_); }
  bool reset() { return stmt_ != nullptr && sqlite3_reset(stmt_) == SQLITE_OK; }
  void finalize()
  {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }

  sqlite3_stmt* get() const { return stmt_; }
  explicit operator bool() const { return stmt_ != nullptr; }

  bool bindText(int index, const std::string& value)
  {
    return sqlite3_bind_text(stmt_, index, value.data(), -1,
                             SQLITE_TRANSIENT) == SQLITE_OK;
  }
  bool bindInt(int index, int value)
  {
    return sqlite3_bind_int(stmt_, index, value) == SQLITE_OK;
  }
  bool bindInt64(int index, int64_t value)
  {
    return sqlite3_bind_int64(stmt_, index, value) == SQLITE_OK;
  }
  bool bindDouble(int index, double value)
  {
    return sqlite3_bind_double(stmt_, index, value) == SQLITE_OK;
  }
  bool bindNull(int index)
  {
    return sqlite3_bind_null(stmt_, index) == SQLITE_OK;
  }
  bool bindBlob(int index, const void* data, size_t size)
  {
    return sqlite3_bind_blob(stmt_, index, data, static_cast<int>(size),
                             SQLITE_TRANSIENT) == SQLITE_OK;
  }

  std::string columnText(int index) const
  {
    const auto* raw =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    return raw ? std::string(raw) : std::string();
  }
  int columnInt(int index) const { return sqlite3_column_int(stmt_, index); }
  int64_t columnInt64(int index) const
  {
    return sqlite3_column_int64(stmt_, index);
  }
  double columnDouble(int index) const
  {
    return sqlite3_column_double(stmt_, index);
  }

private:
  sqlite3_stmt* release()
  {
    sqlite3_stmt* out = stmt_;
    stmt_ = nullptr;
    return out;
  }

  sqlite3_stmt* stmt_ = nullptr;
};
