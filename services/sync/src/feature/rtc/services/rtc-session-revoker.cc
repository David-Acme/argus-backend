#include "rtc-session-revoker.hxx"

#include <feature/rtc/services/rtc-naming.hxx>

#include <drogon/drogon.h>

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

argus::voice::v1::RtcFarewell farewellOf(const FarewellTarget& target)
{
  argus::voice::v1::RtcFarewell farewell;
  farewell.set_room(target.room);
  farewell.set_user_identity(target.identity);
  farewell.set_reason(target.cause);
  return farewell;
}

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
      return revoke({.rooms = std::move(rooms), .voice = std::move(voice), .end = std::move(end),
                     .farewellBudget = kFarewellBudget});
    });
  });
}

drogon::Task<int> RtcSessionRevoker::revoke(RtcRevokeInput input)
{
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + input.farewellBudget;
  const RtcSessionEnd& end = input.end;
  const auto names = co_await input.rooms->listRooms();
  if (!names)
    co_return 0;
  const std::string prefix = rtc_naming::roomPrefixOf(end.userId);
  const std::string userPrefix = rtc_naming::userIdentityPrefixOf(end.userId);
  int removed = 0;
  bool spoke = false;
  for (const auto& room : *names) {
    if (!room.starts_with(prefix))
      continue;
    if (end.sessionId) {
      const std::string identity = rtc_naming::userIdentityOf(end.userId, *end.sessionId);
      if (!co_await input.rooms->silenceParticipant({.room = room, .identity = identity}))
        continue;
      if (input.voice)
        spoke = co_await input.voice->farewell(
                    {.request = farewellOf({.room = room, .identity = identity, .cause = end.cause}),
                     .deadline = remainingOf(deadline)}) ||
                spoke;
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
      spoke = co_await input.voice->farewell(
                  {.request = farewellOf({.room = room, .identity = std::string(), .cause = end.cause}),
                   .deadline = remainingOf(deadline)}) ||
              spoke;
    if (co_await input.rooms->deleteRoom(room))
      ++removed;
  }
  if (removed > 0)
    LOG_INFO << "RTC: removed user " << end.userId << (end.sessionId ? " session" : " (all sessions)")
             << " from " << removed << " call(s) after "
             << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
             << " ms" << (spoke ? " (Argus said goodbye)" : "");
  co_return removed;
}
