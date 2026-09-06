#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.hxx>
#include <feature/api/calendar-event-share/dtos/update-calendar-event-share-dto.hxx>
#include <optional>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/schemas/calendar-event-share/calendar-event-share-schema.hxx>
#include <shared/contracts/user-change-sink.hxx>

struct CalendarEventShareResult
{
  MembershipError error{MembershipError::None};
  std::optional<CalendarEventShareSchema> row;
};

class CalendarEventShareFeatureService
{
public:
  /** Only the owner of the parent record may share it. */
  drogon::Task<CalendarEventShareResult> create(const CreateCalendarEventShareDto& body,
                                    int64_t actorId) const;
  drogon::Task<CalendarEventShareResult> update(int64_t id, const UpdateCalendarEventShareDto& body,
                                    int64_t actorId) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  // The membership row goes to both sides of the share. The parent record goes
  // to the member as well: without it the share would only show up on their
  // device after a full resync.
  void emitMembership(SyncOperation operation, const CalendarEventShareSchema& row,
                      int64_t ownerId) const;
  drogon::Task<void> emitParent(SyncOperation operation, int64_t parentId,
                                int64_t userId) const;

  CalendarEventShareRepository repository_;
  CalendarEventRepository parentRepository_;
  UserRepository userRepository_;
};
