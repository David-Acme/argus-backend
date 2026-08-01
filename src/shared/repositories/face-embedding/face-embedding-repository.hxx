#pragma once

#include "face-embedding-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/face-embedding/face-embedding-schema.hxx>
#include <vector>

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
};
