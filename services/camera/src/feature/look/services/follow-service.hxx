#pragma once

#include <feature/camera-control/services/aim-math.hxx>

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

struct FollowStart
{
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t atMs{0};
};

struct TrackSighting
{
  int64_t cameraId{0};
  int64_t trackId{0};
  NormalizedBox box;
  int64_t atMs{0};
};

struct FollowStep
{
  int64_t cameraId{0};
  AimAngles delta;
};

struct FollowLossQuery
{
  int64_t nowMs{0};
  int64_t lostAfterMs{10000};
};

struct FollowStepInput
{
  const TrackSighting& sighting;
  FieldOfView fov;
  double deadZone{0.05};
  int leadMinSamples{3};
  double leadConsistency{0.7};
};

class FollowBoard
{
public:
  static FollowBoard& instance();

  void start(const FollowStart& start);
  [[nodiscard]] bool following(int64_t cameraId);
  [[nodiscard]] std::optional<FollowStep> sighted(const FollowStepInput& input);
  void settled(int64_t cameraId);
  [[nodiscard]] std::vector<int64_t> lost(const FollowLossQuery& query);
  void stop(int64_t cameraId);

private:
  struct Session
  {
    int64_t trackId{0};
    int64_t startedMs{0};
    int64_t lastSeenMs{0};
    bool moveInFlight{false};
    bool reacquire{false};
    std::deque<TrackSample> samples;
  };

  std::mutex mutex_;
  std::unordered_map<int64_t, Session> sessions_;
};

class FollowService
{
public:
  static void sighting(const TrackSighting& sighting);
  static void sweep();
};
