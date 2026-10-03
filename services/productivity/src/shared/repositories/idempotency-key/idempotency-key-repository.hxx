#pragma once

#include "idempotency-key-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>

class IdempotencyKeyRepository
{
public:
  [[nodiscard]] drogon::Task<std::optional<IdempotencyKeyRecord>>
  find(const IdempotencyKeyFindInput& input) const;

  [[nodiscard]] drogon::Task<void>
  remember(const IdempotencyKeyRememberInput& input) const;
};
