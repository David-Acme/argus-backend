#pragma once

#include <string>

struct sqlite3;

// Executes every statement of a schema file; statement failures log and skip.
bool runSchemaFile(sqlite3* db, const std::string& path);
