#include "rtc-agent-service.hxx"

#include <drogon/drogon.h>

#include <utility>

RtcAgentService::RtcAgentService(RtcAgentInput input)
    : sessions_(*input.sessions),
      config_(std::move(input.config)),
      onCallEnded_(std::move(input.onCallEnded))
{
}

RtcAgentService::~RtcAgentService()
{
  shutdown();
}

RtcJoinRefusal RtcAgentService::validate(const argus::voice::v1::RtcJoin& join)
{
  if (join.room().empty() || join.agent_token().empty() || join.identity().user_id() <= 0)
    return RtcJoinRefusal::Invalid;
  const auto userId = rtc_wire::userIdOfIdentity(join.user_identity());
  if (!userId || *userId != join.identity().user_id())
    return RtcJoinRefusal::Invalid;
  return RtcJoinRefusal::None;
}

void RtcAgentService::reapLocked(std::vector<std::shared_ptr<RtcCall>>& retired)
{
  for (auto it = calls_.begin(); it != calls_.end();) {
    if (it->second->ended()) {
      retired.push_back(std::move(it->second));
      it = calls_.erase(it);
    }
    else
      ++it;
  }
}

void RtcAgentService::joinCall(const argus::voice::v1::RtcJoin& join,
                               const std::function<void(RtcJoinOutcome)>& done)
{
  std::vector<std::shared_ptr<RtcCall>> retired;
  std::shared_ptr<RtcCall> call;
  bool already = false;
  {
    std::scoped_lock lock(mutex_);
    reapLocked(retired);
    if (closed_) {
      already = false;
    }
    else if (calls_.contains(join.room())) {
      already = true;
    }
    else {
      call = std::make_shared<RtcCall>(RtcCallInput{
          .sessions = &sessions_,
          .join = join,
          .url = config_.url,
          .timings = config_.timings,
          .onJoined = [done](bool joined) { done({.joined = joined, .already = false}); },
          .onEnded = onCallEnded_});
      calls_.emplace(join.room(), call);
    }
  }
  retired.clear();
  if (already) {
    LOG_INFO << "Voice: RTC agent already in " << join.room();
    done({.joined = true, .already = true});
    return;
  }
  if (!call) {
    done({.joined = false, .already = false});
    return;
  }
  call->run();
}

void RtcAgentService::joinRoom(const argus::voice::v1::RtcJoin& join,
                               std::function<void(grpc::Status, argus::voice::v1::RtcJoined)> done)
{
  if (validate(join) != RtcJoinRefusal::None) {
    done({grpc::StatusCode::INVALID_ARGUMENT, "room, agent token and a matching user identity are required"},
         {});
    return;
  }
  joinCall(join, [done = std::move(done)](RtcJoinOutcome outcome) {
    argus::voice::v1::RtcJoined joined;
    joined.set_joined(outcome.joined);
    joined.set_already(outcome.already);
    done(outcome.joined ? grpc::Status::OK
                        : grpc::Status(grpc::StatusCode::UNAVAILABLE, "the agent could not join the room"),
         joined);
  });
}

void RtcAgentService::farewellRoom(const argus::voice::v1::RtcFarewell& farewell,
                                   std::function<void()> done)
{
  std::shared_ptr<RtcCall> call;
  {
    std::scoped_lock lock(mutex_);
    const auto it = calls_.find(farewell.room());
    if (it != calls_.end() && !it->second->ended())
      call = it->second;
  }
  if (!call) {
    done();
    return;
  }
  call->farewell(farewell, std::move(done));
}

size_t RtcAgentService::activeCalls() const
{
  std::scoped_lock lock(mutex_);
  size_t active = 0;
  for (const auto& [room, call] : calls_)
    if (!call->ended())
      ++active;
  return active;
}

void RtcAgentService::shutdown()
{
  std::unordered_map<std::string, std::shared_ptr<RtcCall>> calls;
  {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    calls.swap(calls_);
  }
  for (auto& [room, call] : calls)
    call->requestStop(rtc_wire::DoneReason::Error);
  for (auto& [room, call] : calls)
    call->waitEnded();
}
