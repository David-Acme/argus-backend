#include "voice-call-tracker.hxx"

#include <algorithm>
#include <utility>

namespace
{
constexpr size_t kClosedMemory = 256;

bool taints(TurnVerdict verdict)
{
  return verdict == TurnVerdict::MixedTurn ||
         verdict == TurnVerdict::OtherSpeaker || verdict == TurnVerdict::Drift ||
         verdict == TurnVerdict::CallMismatch;
}
}

VoiceCallTracker::VoiceCallTracker(PassiveVoiceConfig config)
    : policy_(config)
{
}

TurnVerdict VoiceCallTracker::observe(TrackedTurn turn)
{
  const std::scoped_lock lock(mutex_);
  if (recentlyClosed(turn.callKey))
    return TurnVerdict::CallClosed;
  auto found = calls_.find(turn.callKey);
  if (found == calls_.end()) {
    if (std::cmp_greater_equal(calls_.size(), policy_.config().maxOpenCalls))
      return TurnVerdict::Busy;
    found = calls_
                .emplace(turn.callKey, ClosedCall{.callKey = turn.callKey,
                                                  .userId = turn.userId,
                                                  .deviceHash = turn.deviceHash,
                                                  .turns = {},
                                                  .speechSeconds = 0.0F,
                                                  .tainted = false,
                                                  .taint = TurnVerdict::Accepted,
                                                  .rejectedTurns = 0,
                                                  .startedAt = turn.now,
                                                  .lastTurnAt = turn.now})
                .first;
  }
  ClosedCall& call = found->second;
  call.lastTurnAt = std::max(call.lastTurnAt, turn.now);
  if (call.tainted) {
    ++call.rejectedTurns;
    return TurnVerdict::CallTainted;
  }

  TurnVerdict verdict = TurnVerdict::CallMismatch;
  if (call.userId == turn.userId && call.deviceHash == turn.deviceHash)
    verdict = policy_.judgeTurn({.accepted = call.turns,
                                 .embedding = turn.embedding,
                                 .halvesScore = turn.halvesScore,
                                 .bestOther = turn.bestOther,
                                 .ownScore = turn.ownScore});
  if (verdict == TurnVerdict::Accepted) {
    call.speechSeconds += turn.speechSeconds;
    call.turns.push_back(std::move(turn.embedding));
    return verdict;
  }
  ++call.rejectedTurns;
  if (taints(verdict)) {
    call.tainted = true;
    call.taint = verdict;
    call.turns.clear();
  }
  return verdict;
}

std::optional<ClosedCall> VoiceCallTracker::close(const std::string& callKey)
{
  const std::scoped_lock lock(mutex_);
  rememberClosed(callKey);
  auto found = calls_.find(callKey);
  if (found == calls_.end())
    return std::nullopt;
  ClosedCall call = std::move(found->second);
  calls_.erase(found);
  return call;
}

std::vector<ClosedCall> VoiceCallTracker::expire(int64_t now)
{
  const std::scoped_lock lock(mutex_);
  std::vector<ClosedCall> expired;
  for (auto it = calls_.begin(); it != calls_.end();) {
    if (now - it->second.lastTurnAt < policy_.config().callIdleSeconds) {
      ++it;
      continue;
    }
    rememberClosed(it->first);
    expired.push_back(std::move(it->second));
    it = calls_.erase(it);
  }
  return expired;
}

size_t VoiceCallTracker::open() const
{
  const std::scoped_lock lock(mutex_);
  return calls_.size();
}

void VoiceCallTracker::rememberClosed(const std::string& callKey)
{
  if (recentlyClosed(callKey))
    return;
  closed_.push_back(callKey);
  if (closed_.size() > kClosedMemory)
    closed_.pop_front();
}

bool VoiceCallTracker::recentlyClosed(const std::string& callKey) const
{
  return std::ranges::find(closed_, callKey) != closed_.end();
}
