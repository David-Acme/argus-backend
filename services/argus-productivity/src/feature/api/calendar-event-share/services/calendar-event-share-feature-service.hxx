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
  struct UpdateInput
  {
    int64_t id{0};
    const UpdateCalendarEventShareDto& body;
    int64_t actorId{0};
  };

  /** Only the owner of the parent record may share it. */
  drogon::Task<CalendarEventShareResult> create(const CreateCalendarEventShareDto& body,
                                    int64_t actorId) const;
  drogon::Task<CalendarEventShareResult> update(const UpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id, int64_t actorId) const;

private:
  struct EmitMembershipInput
  {
    SyncOperation operation{};
    const CalendarEventShareSchema& row;
    int64_t ownerId{0};
  };

  struct EmitParentInput
  {
    SyncOperation operation{};
    int64_t parentId{0};
    int64_t userId{0};
  };

  // Emits the membership row to both sides and the parent record to the member.
  void emitMembership(const EmitMembershipInput& input) const;
  drogon::Task<void> emitParent(const EmitParentInput& input) const;

  CalendarEventShareRepository repository_;
  CalendarEventRepository parentRepository_;
  UserRepository userRepository_;
};
