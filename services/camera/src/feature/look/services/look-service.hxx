#pragma once

#include <feature/camera-control/services/aim-math.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <shared/repositories/camera/camera-repository.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>
#include <vector>

struct LookRequest
{
  int64_t cameraId{0};
  int64_t trackId{0};
  NormalizedBox box;
  std::string reason;
  int64_t budgetMs{0};
  bool follow{false};
};

struct LookOutcome
{
  bool confirmed{false};
  std::string cls;
  float confidence{0.0F};
  std::vector<int64_t> cropIds;
  bool moved{false};
  std::string detail;
};

struct LookPlanInput
{
  NormalizedBox box;
  AimAngles position;
  bool positionKnown{false};
  bool lookable{false};
  const std::vector<NoAimSector>& sectors;
  const CameraPtzConfig& ptz;
};

struct LookPlan
{
  bool move{false};
  AimAngles delta;
  std::string refusal;
};

namespace look_plan
{
[[nodiscard]] LookPlan plan(const LookPlanInput& input);
[[nodiscard]] NormalizedBox centered(const NormalizedBox& box);
}

class LookService
{
public:
  [[nodiscard]] drogon::Task<LookOutcome> look(LookRequest request) const;

private:
  CameraControlFeatureService control_;
  CameraRepository repository_;
};
