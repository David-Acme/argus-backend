#include "rtc-session-revoker.hxx"

#include <feature/rtc/services/rtc-naming.hxx>

#include <drogon/drogon.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace
{

std::chrono::milliseconds remainingOf(std::chrono::steady_clock::time_point deadline)
{
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

struct FarewellTarget
{
  const std::string& room;
  const std::string& identity;
  const std::string& cause;
};

struct OnlyHumanInput
{
  const LiveKitRoomClient& rooms;
  const std::string& room;
  const std::string& identity;
};

drogon::Task<bool> onlyHumanIn(OnlyHumanInput input)
{
  const auto participants = co_await input.rooms.listParticipants(input.room);
  if (!participants)
    co_return false;
  co_return std::ranges::none_of(*participants, [&input](const std::string& participant) {
    return participant != input.identity && participant != rtc_naming::kAgentIdentity;
  });
}

argus::voice::v1::RtcFarewell farewellOf(const FarewellTarget& target)
{
  argus::voice::v1::RtcFarewell farewell;
  farewell.set_room(target.room);
  farewell.set_user_identity(target.identity);
  farewell.set_reason(target.cause);
  return farewell;
}

}

std::vector<std::chrono::milliseconds> RtcSessionRevoker::defaultListRetries()
{
  return {std::chrono::milliseconds(500), std::chrono::milliseconds(1500),
          std::chrono::milliseconds(4000)};
}

RtcSessionRevoker::RtcSessionRevoker(std::shared_ptr<const LiveKitRoomClient> rooms,
                                     std::shared_ptr<const RtcVoiceJoiner> voice)
    : rooms_(std::move(rooms)), voice_(std::move(voice))
{
}

void RtcSessionRevoker::sessionEnded(RtcSessionEnd end) const
{
  if (!rooms_ || end.userId <= 0)
    return;
  drogon::app().getLoop()->queueInLoop([rooms = rooms_, voice = voice_, end = std::move(end)]() mutable {
    drogon::async_run([rooms = std::move(rooms), voice = std::move(voice), end = std::move(end)]() mutable {
      return revoke({.rooms = std::move(rooms),
                     .voice = std::move(voice),
                     .end = std::move(end),
                     .farewellBudget = kFarewellBudget,
                     .listRetries = defaultListRetries()});
    });
  });
}

drogon::Task<int> RtcSessionRevoker::revoke(RtcRevokeInput input)
{
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + input.farewellBudget;
  const RtcSessionEnd& end = input.end;
  auto names = co_await input.rooms->listRooms();
  for (const auto backoff : input.listRetries) {
    if (names)
      break;
    if (backoff.count() > 0)
      co_await drogon::sleepCoro(drogon::app().getLoop(),
                                 std::chrono::duration<double>(backoff));
    names = co_await input.rooms->listRooms();
  }
  if (!names) {
    LOG_WARN << "RTC: could not list the rooms to revoke user " << end.userId
             << "; their calls were left as they were";
    co_return 0;
  }
  const std::string prefix = rtc_naming::roomPrefixOf(end.userId);
  const std::string userPrefix = rtc_naming::userIdentityPrefixOf(end.userId);
  int removed = 0;
  for (const auto& room : *names) {
    if (!room.starts_with(prefix))
      continue;
    if (end.sessionId) {
      const std::string identity = rtc_naming::userIdentityOf(end.userId, *end.sessionId);
      if (!co_await input.rooms->silenceParticipant({.room = room, .identity = identity}))
        continue;
      if (input.voice)
        std::ignore = co_await input.voice->farewell(
            {.request = farewellOf({.room = room, .identity = identity, .cause = end.cause}),
             .deadline = remainingOf(deadline)});
      if (co_await onlyHumanIn({.rooms = *input.rooms, .room = room, .identity = identity})) {
        if (co_await input.rooms->deleteRoom(room))
          ++removed;
        continue;
      }
      if (co_await input.rooms->removeParticipant({.room = room, .identity = identity}))
        ++removed;
      continue;
    }
    if (const auto participants = co_await input.rooms->listParticipants(room)) {
      for (const auto& identity : *participants)
        if (identity.starts_with(userPrefix))
          co_await input.rooms->silenceParticipant({.room = room, .identity = identity});
    }
    if (input.voice)
      std::ignore = co_await input.voice->farewell(
          {.request = farewellOf({.room = room, .identity = std::string(), .cause = end.cause}),
           .deadline = remainingOf(deadline)});
    if (co_await input.rooms->deleteRoom(room))
      ++removed;
  }
  if (removed > 0)
    LOG_INFO << "RTC: removed user " << end.userId << (end.sessionId ? " session" : " (all sessions)")
             << " from " << removed << " call(s) after "
             << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
             << " ms";
  co_return removed;
}
