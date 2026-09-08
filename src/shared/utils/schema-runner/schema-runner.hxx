#pragma once

#include <string>

struct sqlite3;

// Executes every statement of a schema file on the connection; false when the file cannot be opened.
bool runSchemaFile(sqlite3* db, const std::string& path);
