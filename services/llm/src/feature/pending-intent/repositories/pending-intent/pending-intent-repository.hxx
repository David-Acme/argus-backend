#pragma once

#include "pending-intent-query.hxx"

#include <optional>
#include <string>
#include <vector>

struct sqlite3;

class PendingIntentRepository
{
public:
  [[nodiscard]] int64_t offer(sqlite3* db, const PendingIntentCreateInput& input) const;
  [[nodiscard]] std::vector<int64_t> accept(sqlite3* db, const PendingIntentAcceptInput& input) const;
  [[nodiscard]] std::optional<PendingIntentRow> find(sqlite3* db, int64_t id) const;
  [[nodiscard]] std::vector<PendingIntentRow> waiting(sqlite3* db, const std::string& module) const;
  [[nodiscard]] std::vector<PendingIntentRow> waitingBefore(sqlite3* db, int64_t before) const;
  bool settle(sqlite3* db, const PendingIntentSettleInput& input) const;
  int expireOffers(sqlite3* db, int64_t before, int64_t at) const;
  int purge(sqlite3* db, int64_t before) const;
};
