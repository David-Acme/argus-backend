#pragma once

#include "visitor-notice-query.hxx"

#include <cstdint>

struct sqlite3;

class VisitorNoticeRepository
{
public:
  [[nodiscard]] int64_t record(sqlite3* db, const VisitorNoticeWriteInput& input) const;
  bool remove(sqlite3* db, int64_t id) const;
  [[nodiscard]] int64_t countSince(sqlite3* db, const VisitorNoticeCountInput& input) const;
  int purge(sqlite3* db, int64_t before) const;
};
