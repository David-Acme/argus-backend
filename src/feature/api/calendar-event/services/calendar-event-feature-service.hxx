#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/api/calendar-event/dtos/update-calendar-event-dto.hxx>
#include <optional>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/schemas/calendar-event/calendar-event-schema.hxx>
#include <shared/contracts/user-change-sink.hxx>

struct CalendarEventOwnerInput
{
  int64_t ownerId{0};
  int64_t actorId{0};
};

class CalendarEventFeatureService
{
public:
  drogon::Task<CalendarEventSchema> create(const CreateCalendarEventDto& body,
                                           const CalendarEventOwnerInput& who) const;
  drogon::Task<std::optional<CalendarEventSchema>>
  update(int64_t id, const UpdateCalendarEventDto& body, int64_t actorId) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  // Pushes every mutation to the owner and to everyone the event is shared with.
  drogon::Task<void> emit(SyncOperation operation,
                          const CalendarEventSchema& row) const;
  /** True for the owner and for a member whose share says `edit`. */
  drogon::Task<bool> canEdit(const CalendarEventSchema& row,
                             int64_t actorId) const;

  CalendarEventRepository repository_;
  CalendarEventShareRepository shareRepository_;
};
