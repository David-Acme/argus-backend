#pragma once

#include <string>

struct sqlite3;

bool runSchemaFile(sqlite3* db, const std::string& path);
