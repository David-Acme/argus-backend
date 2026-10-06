#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/reminder/dtos/create-reminder-dto.hxx>
#include <feature/reminder/dtos/update-reminder-dto.hxx>
#include <optional>
#include <shared/repositories/idempotency-key/idempotency-key-repository.hxx>
#include <shared/repositories/reminder/reminder-repository.hxx>
#include <shared/schemas/reminder/reminder-schema.hxx>
#include <string>
#include <sync/user-change-sink.hxx>
#include <vector>

struct ReminderCreateCommand
{
  const CreateReminderDto& body;
  int64_t userId{0};
  std::string idempotencyKey;
};

struct ReminderUpdateCommand
{
  int64_t id{0};
  const UpdateReminderDto& body;
  int64_t userId{0};
};

struct ReminderRef
{
  int64_t id{0};
  int64_t userId{0};
};

struct ReminderListCommand
{
  int64_t userId{0};
  bool includeCompleted{true};
  int64_t limit{reminder_query::kListLimit};
};

class ReminderFeatureService
{
public:
  drogon::Task<ReminderSchema> create(const ReminderCreateCommand& command) const;
  drogon::Task<std::optional<ReminderSchema>>
  update(const ReminderUpdateCommand& command) const;
  drogon::Task<bool> remove(const ReminderRef& ref) const;
  drogon::Task<std::optional<ReminderSchema>> get(const ReminderRef& ref) const;
  drogon::Task<std::vector<ReminderSchema>>
  list(const ReminderListCommand& command) const;

private:
  struct EmitInput
  {
    SyncOperation operation{};
    const ReminderSchema& row;
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> emit(const EmitInput& input) const;

  ReminderRepository repository_;
  IdempotencyKeyRepository idempotency_;
};
