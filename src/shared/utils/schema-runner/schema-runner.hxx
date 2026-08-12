#pragma once

#include <string>

struct sqlite3;

// Executes every statement of a schema file (comments and blanks skipped) on
// the given connection. database/schema.sql is the single source of table
// DDL: connection owners (DbService, VecDb, SqliteGraph, JobRepository) run
// it at open, so every connection — including lab temp databases — gets the
// same schema. Returns false when the file cannot be opened; statement
// failures are logged and skipped.
bool runSchemaFile(sqlite3* db, const std::string& path);
