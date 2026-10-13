#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/repositories/reminder/reminder-repository.hxx>

struct SeriesAdvanceInput
{
  int64_t reminderId{0};
  int64_t targetUserId{0};
  int64_t moment{0};
  drogon::orm::DbClient* client{nullptr};
};

class ReminderSeries
{
public:
  [[nodiscard]] drogon::Task<std::optional<int64_t>> advance(const SeriesAdvanceInput& input) const;

private:
  ReminderRepository repository_;
};
