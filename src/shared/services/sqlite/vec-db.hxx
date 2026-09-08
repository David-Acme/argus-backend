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
  // The caller names its schema: the vector database is domain-neutral and
  // must not know which service's tables it is applying.
  void applySchema(const std::string& schemaFile);
  int embeddingDims() const;
  bool schemaOutdated();
  void recreateMemoryVecTable();
  void recreateVecTables();

private:
  std::mutex mutex_;
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> db_{nullptr, &sqlite3_close};
  VectorIndexRepository repo_;
};
