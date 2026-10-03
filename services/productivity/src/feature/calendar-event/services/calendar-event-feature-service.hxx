#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/calendar-event/dtos/update-calendar-event-dto.hxx>
#include <optional>
#include <string>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/idempotency-key/idempotency-key-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/schemas/calendar-event/calendar-event-schema.hxx>
#include <sync/user-change-sink.hxx>

struct CalendarEventOwnerInput
{
  int64_t ownerId{0};
  int64_t actorId{0};
  std::string idempotencyKey;
};

class CalendarEventFeatureService
{
public:
  struct UpdateInput
  {
    int64_t id{0};
    const UpdateCalendarEventDto& body;
    int64_t actorId{0};
  };

  drogon::Task<CalendarEventSchema> create(const CreateCalendarEventDto& body,
                                           const CalendarEventOwnerInput& who) const;
  drogon::Task<std::optional<CalendarEventSchema>>
  update(const UpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  struct EmitInput
  {
    SyncOperation operation{};
    const CalendarEventSchema& row;
    drogon::orm::DbClient* client{nullptr};
  };

  struct CanEditInput
  {
    const CalendarEventSchema& row;
    int64_t actorId{0};
    drogon::orm::DbClient* client{nullptr};
  };

  drogon::Task<void> emit(const EmitInput& input) const;
  drogon::Task<bool> canEdit(const CanEditInput& input) const;

  CalendarEventRepository repository_;
  CalendarEventShareRepository shareRepository_;
  IdempotencyKeyRepository idempotency_;
};
