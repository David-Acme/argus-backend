#pragma once

#include <feature/camera-control/services/aim-math.hxx>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class AimActivity : uint8_t
{
  Idle = 0,
  Looking,
  Following,
  Patrolling,
  Returning
};

std::string aimActivityToString(AimActivity activity);

struct AimMoveRecord
{
  int64_t cameraId{0};
  AimAngles delta;
  int64_t atMs{0};
};

struct AimActivityChange
{
  int64_t cameraId{0};
  AimActivity activity{AimActivity::Idle};
};

struct AimWearQuery
{
  int64_t cameraId{0};
  int64_t nowMs{0};
  int perMinute{6};
  int64_t dailyCap{400};
};

struct AimSnapshot
{
  AimAngles position;
  bool known{true};
  bool atHome{true};
  AimActivity activity{AimActivity::Idle};
  int64_t movesToday{0};
};

class AimBoard
{
public:
  static AimBoard& instance();

  [[nodiscard]] bool wearAllows(const AimWearQuery& query);
  void recordMove(const AimMoveRecord& move);
  void arriveHome(int64_t cameraId);
  void lose(int64_t cameraId);
  void setActivity(const AimActivityChange& change);
  [[nodiscard]] AimSnapshot snapshot(int64_t cameraId);
  void forget(int64_t cameraId);

private:
  struct Entry
  {
    AimAngles position;
    bool known{true};
    bool atHome{true};
    AimActivity activity{AimActivity::Idle};
    std::deque<int64_t> recentMovesMs;
    int64_t movesToday{0};
    int64_t dayIndex{0};
  };

  static int64_t dayOf(int64_t atMs);

  std::mutex mutex_;
  std::unordered_map<int64_t, Entry> entries_;
};
