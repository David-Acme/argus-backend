#include "aim-board.hxx"

#include <trantor/utils/Logger.h>

#include <vector>

namespace
{
constexpr int64_t kDayMs = 86400000;
constexpr int64_t kMinuteMs = 60000;
}

std::string aimActivityToString(AimActivity activity)
{
  switch (activity) {
    case AimActivity::Idle:
      return "idle";
    case AimActivity::Looking:
      return "looking";
    case AimActivity::Following:
      return "following";
    case AimActivity::Patrolling:
      return "patrolling";
    case AimActivity::Returning:
      return "returning";
  }
  return "idle";
}

AimBoard& AimBoard::instance()
{
  static AimBoard board;
  return board;
}

int64_t AimBoard::dayOf(int64_t atMs)
{
  return atMs / kDayMs;
}

bool AimBoard::wearAllows(const AimWearQuery& query)
{
  const std::scoped_lock lock(mutex_);
  Entry& entry = entries_[query.cameraId];
  if (entry.dayIndex != dayOf(query.nowMs)) {
    entry.dayIndex = dayOf(query.nowMs);
    entry.movesToday = 0;
  }
  while (!entry.recentMovesMs.empty() && query.nowMs - entry.recentMovesMs.front() >= kMinuteMs)
    entry.recentMovesMs.pop_front();
  const std::vector<int64_t> recent(entry.recentMovesMs.begin(), entry.recentMovesMs.end());
  const bool allowed = aim_math::wearAllows({.recentMovesMs = recent,
                                             .nowMs = query.nowMs,
                                             .perMinute = query.perMinute,
                                             .movesToday = entry.movesToday,
                                             .dailyCap = query.dailyCap});
  if (!allowed && query.dailyCap > 0 && entry.movesToday >= query.dailyCap)
    LOG_WARN << "Camera look: camera " << query.cameraId << " reached its daily cap of " << query.dailyCap
             << " automatic moves";
  return allowed;
}

void AimBoard::recordMove(const AimMoveRecord& move)
{
  const std::scoped_lock lock(mutex_);
  Entry& entry = entries_[move.cameraId];
  if (entry.dayIndex != dayOf(move.atMs)) {
    entry.dayIndex = dayOf(move.atMs);
    entry.movesToday = 0;
  }
  entry.position.pan += move.delta.pan;
  entry.position.tilt += move.delta.tilt;
  entry.atHome = false;
  entry.recentMovesMs.push_back(move.atMs);
  ++entry.movesToday;
}

void AimBoard::arriveHome(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  Entry& entry = entries_[cameraId];
  entry.position = {};
  entry.known = true;
  entry.atHome = true;
  entry.activity = AimActivity::Idle;
}

void AimBoard::lose(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  Entry& entry = entries_[cameraId];
  entry.position = {};
  entry.known = false;
  entry.atHome = false;
}

void AimBoard::setActivity(const AimActivityChange& change)
{
  const std::scoped_lock lock(mutex_);
  entries_[change.cameraId].activity = change.activity;
}

AimSnapshot AimBoard::snapshot(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  const Entry& entry = entries_[cameraId];
  return {.position = entry.position,
          .known = entry.known,
          .atHome = entry.atHome,
          .activity = entry.activity,
          .movesToday = entry.movesToday};
}

void AimBoard::forget(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  entries_.erase(cameraId);
}
