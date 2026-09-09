#pragma once

#include <memory>
#include <mutex>
#include <shared/repositories/vector-index/vector-index-repository.hxx>
#include <sqlite3.h>
#include <string>

class VecDb
{
public:
  VecDb() = default;
  ~VecDb() = default;

  VecDb(const VecDb&) = delete;
  VecDb& operator=(const VecDb&) = delete;

  static VecDb& instance();

  std::mutex& mutex();
  sqlite3* handle();
  // Empty keeps the [database] file fallback.
  void setDbFile(std::string file);
  // The vector database is domain-neutral and names no table itself.
  void applySchema(const std::string& schemaFile);
  int embeddingDims() const;
  bool schemaOutdated();
  void recreateMemoryVecTable();
  void recreateVecTables();

private:
  std::string dbFile_;

  std::mutex mutex_;
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> db_{nullptr, &sqlite3_close};
  VectorIndexRepository repo_;
};
