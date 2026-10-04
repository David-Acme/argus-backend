#include "rtc-session-revoker.hxx"

#include <feature/rtc/services/rtc-naming.hxx>

#include <drogon/drogon.h>

#include <utility>

RtcSessionRevoker::RtcSessionRevoker(std::shared_ptr<const LiveKitRoomClient> rooms) : rooms_(std::move(rooms)) {}

void RtcSessionRevoker::sessionEnded(RtcSessionEnd end) const
{
  if (!rooms_ || end.userId <= 0)
    return;
  drogon::app().getLoop()->queueInLoop([rooms = rooms_, end = std::move(end)]() mutable {
    drogon::async_run([rooms = std::move(rooms), end = std::move(end)]() mutable {
      return revoke(std::move(rooms), std::move(end));
    });
  });
}

drogon::Task<int> RtcSessionRevoker::revoke(std::shared_ptr<const LiveKitRoomClient> rooms, RtcSessionEnd end)
{
  const auto names = co_await rooms->listRooms();
  if (!names)
    co_return 0;
  const std::string prefix = rtc_naming::roomPrefixOf(end.userId);
  int removed = 0;
  for (const auto& room : *names) {
    if (!room.starts_with(prefix))
      continue;
    if (end.sessionId) {
      if (co_await rooms->removeParticipant(
              {.room = room, .identity = rtc_naming::userIdentityOf(end.userId, *end.sessionId)}))
        ++removed;
    }
    else if (co_await rooms->deleteRoom(room)) {
      ++removed;
    }
  }
  if (removed > 0)
    LOG_INFO << "RTC: removed user " << end.userId << (end.sessionId ? " session" : " (all sessions)")
             << " from " << removed << " call(s)";
  co_return removed;
}
