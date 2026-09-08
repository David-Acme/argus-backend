#pragma once

#include <string>

struct sqlite3;

// Executes every statement of a schema file (comments and blanks skipped) on
// the connection. Returns false when the file cannot be opened; statement
// failures are logged and skipped.
bool runSchemaFile(sqlite3* db, const std::string& path);
