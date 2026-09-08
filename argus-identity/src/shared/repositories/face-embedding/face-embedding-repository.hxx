#pragma once

#include "face-embedding-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/face-embedding/face-embedding-schema.hxx>
#include <vector>

struct sqlite3;

class FaceEmbeddingRepository
{
public:
  FaceEmbeddingRepository() = default;
  ~FaceEmbeddingRepository() = default;

  drogon::Task<std::optional<FaceEmbeddingSchema>> findById(int64_t id) const;

  drogon::Task<std::vector<FaceEmbeddingSchema>>
  findByPerson(int64_t personId) const;

  drogon::Task<FaceEmbeddingSchema>
  create(const FaceEmbeddingCreateInput& input) const;

  drogon::Task<bool> removeByPerson(int64_t personId) const;

  drogon::Task<std::vector<FaceEmbeddingSchema>> findAll() const;

  std::vector<int64_t> findIdsByPerson(sqlite3* db, int64_t personId) const;
  bool insertVec(sqlite3* db, const FaceVecInsertInput& input) const;
  std::vector<FaceVecHit> searchVec(sqlite3* db,
                                    const FaceVecSearchInput& input) const;
  bool deleteVecRow(sqlite3* db, int64_t rowid) const;
  size_t countVec(sqlite3* db) const;
};
