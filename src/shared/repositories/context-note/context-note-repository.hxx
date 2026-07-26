#pragma once

#include "context-note-query.hxx"
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/context-note/context-note-schema.hxx>
#include <vector>

class ContextNoteRepository
{
public:
  ContextNoteRepository() = default;
  ~ContextNoteRepository() = default;

  drogon::Task<std::optional<ContextNoteSchema>>
  findById(int64_t id) const;
  drogon::Task<std::vector<ContextNoteSchema>> findActive() const;
  drogon::Task<ContextNoteSchema>
  create(const ContextNoteCreateInput& input) const;
  drogon::Task<ContextNoteSchema>
  update(int64_t id, const ContextNoteUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;
};
