#pragma once

#include "relay-thread-query.hxx"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

class RelayThreadRepository
{
public:
  [[nodiscard]] int64_t create(sqlite3* db, const RelayThreadCreateInput& input) const;
  [[nodiscard]] std::optional<RelayThreadRow> openFor(sqlite3* db, const RelayThreadOpenInput& input) const;
  [[nodiscard]] std::vector<RelayThreadRow> openThreads(sqlite3* db, const RelayThreadOpenInput& input) const;
  [[nodiscard]] int64_t countFrom(sqlite3* db, const RelayThreadCountInput& input) const;
  bool answer(sqlite3* db, int64_t id) const;
  bool setDelivery(sqlite3* db, const RelayThreadDeliveryInput& input) const;
  bool remove(sqlite3* db, int64_t id) const;
  [[nodiscard]] std::vector<std::string> expire(sqlite3* db, int64_t now) const;
  int purge(sqlite3* db, int64_t before) const;
};
