#pragma once

#include "outbox-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <vector>

namespace outbox
{

class OutboxRepository
{
public:
  explicit OutboxRepository(ClientAccessor client);

  [[nodiscard]] bool migrateSchema() const;

  [[nodiscard]] drogon::Task<OutboxDisposition>
  insert(const OutboxInsert& input) const;

  [[nodiscard]] std::vector<OutboxRow> pendingBatch(int limit) const;

  [[nodiscard]] bool markSent(int64_t id, int64_t at) const;

  [[nodiscard]] bool recordAttempt(int64_t id) const;

  [[nodiscard]] int64_t purgeSent(int64_t olderThanMs) const;

private:
  [[nodiscard]] drogon::orm::DbClientPtr client() const;
  [[nodiscard]] bool hasColumn(const char* column) const;

  ClientAccessor client_;
};

}
