#pragma once

#include <config/camera-config.hxx>
#include <feature/camera-control/services/aim-math.hxx>
#include <feature/objects/object-detector.hxx>
#include <shared/services/stream/frame-source.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SecondLookDependencies
{
  IObjectDetector* detector{nullptr};
  IFrameSource* source{nullptr};
  CameraLookConfig config;
};

struct SecondLookInput
{
  int64_t cameraId{0};
  std::string cameraName;
  NormalizedBox box;
  int grabs{3};
  int64_t budgetMs{3000};
  std::vector<NoAimSector> sectors;
  AimAngles position;
  bool awayFromHome{false};
  FieldOfView fov;
};

struct SecondLookResult
{
  bool looked{false};
  bool confirmed{false};
  std::string cls;
  float confidence{0.0F};
  std::vector<std::string> crops;
  int64_t capturedAtMs{0};
};

struct DoubtInput
{
  float confidence{0.0F};
  float confirmConfidence{0.45F};
  double boxShare{0.0};
  double smallBoxShare{0.03};
  bool unknownPerson{false};
  bool protect{false};
};

struct LookCooldownKey
{
  int64_t cameraId{0};
  int cell{0};
  int64_t atMs{0};
};

namespace second_look
{
[[nodiscard]] bool doubtful(const DoubtInput& input);
}

class LookCooldown
{
public:
  static LookCooldown& instance();

  [[nodiscard]] bool cooling(const LookCooldownKey& key);
  void cool(const LookCooldownKey& key, int64_t forMs);

private:
  std::mutex mutex_;
  std::unordered_map<int64_t, int64_t> until_;
};

class SecondLook
{
public:
  explicit SecondLook(SecondLookDependencies dependencies);

  [[nodiscard]] drogon::Task<SecondLookResult> confirm(SecondLookInput input) const;

  [[nodiscard]] const CameraLookConfig& config() const { return dependencies_.config; }

  static void install(const SecondLook* look);
  [[nodiscard]] static const SecondLook* installed();

private:
  struct CropVerdict
  {
    bool decoded{false};
    std::string cls;
    float confidence{0.0F};
    std::string jpeg;
  };

  struct CropInput
  {
    std::vector<uint8_t> jpeg;
    const SecondLookInput& look;
  };

  [[nodiscard]] CropVerdict judge(const CropInput& input) const;

  SecondLookDependencies dependencies_;
};
