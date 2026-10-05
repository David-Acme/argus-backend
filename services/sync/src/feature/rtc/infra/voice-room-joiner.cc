#include "voice-room-joiner.hxx"

#include <drogon/drogon.h>
#include <runtime/blocking-task.hxx>

#include <utility>

VoiceRoomJoiner::VoiceRoomJoiner(std::shared_ptr<const VoiceClient> client) : client_(std::move(client)) {}

drogon::Task<bool> VoiceRoomJoiner::join(argus::voice::v1::RtcJoin join) const
{
  const VoiceRoomJoinResult result = co_await BlockingTask<VoiceRoomJoinResult>{
      [client = client_, request = std::move(join)] { return client->joinRoom(request); }};
  if (!result.status.ok())
    LOG_WARN << "RTC: argus-voice JoinRoom refused: " << result.status.error_message();
  co_return result.status.ok() && result.joined;
}

drogon::Task<bool> VoiceRoomJoiner::farewell(RtcFarewellInput input) const
{
  if (input.deadline.count() <= 0)
    co_return false;
  co_return co_await BlockingTask<bool>{[client = client_, farewell = std::move(input)] {
    return client->farewell(farewell.request, static_cast<int>(farewell.deadline.count()));
  }};
}
