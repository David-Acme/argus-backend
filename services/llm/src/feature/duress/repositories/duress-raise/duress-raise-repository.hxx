#pragma once

#include "duress-raise-query.hxx"

#include <cstdint>
#include <optional>
#include <vector>

struct sqlite3;

class DuressRaiseRepository
{
public:
  [[nodiscard]] std::optional<int64_t> add(sqlite3* db, const DuressRaiseCreateInput& input) const;
  [[nodiscard]] std::vector<DuressRaiseRow> pending(sqlite3* db) const;
  bool remove(sqlite3* db, int64_t id) const;
};
