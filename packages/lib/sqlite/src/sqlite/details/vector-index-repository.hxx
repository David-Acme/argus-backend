#pragma once

#include <string>

struct sqlite3;

class VectorIndexRepository
{
public:
  bool createTables(sqlite3* db, int dims);
  bool dropTables(sqlite3* db);
  bool recreateMemoryVec(sqlite3* db, int dims);
  bool recreateVecTables(sqlite3* db, int dims);
  std::string tableSql(sqlite3* db, const std::string& name);
  bool hasCosineMetric(sqlite3* db);
  bool memoryVecMatches(sqlite3* db, int dims);
};
