#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/api/calendar-event/dtos/update-calendar-event-dto.hxx>
#include <optional>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/schemas/calendar-event/calendar-event-schema.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>

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
  // Writes go over REST, reads come back through /sync: every mutation pushes
  // the row to the owner and to everyone the event is shared with, so a shared
  // calendar updates on every device without polling.
  drogon::Task<void> emit(SyncOperation operation,
                          const CalendarEventSchema& row) const;
  /** True for the owner and for a member whose share says `edit`. */
  drogon::Task<bool> canEdit(const CalendarEventSchema& row,
                             int64_t actorId) const;

  CalendarEventRepository repository_;
  CalendarEventShareRepository shareRepository_;
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
