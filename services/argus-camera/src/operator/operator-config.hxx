#pragma once

#include <operator/event-intelligence.hxx>

#include <cstdint>
#include <string>
#include <vector>

// [objects] — the detection capacity.
struct ObjectsConfig
{
  bool enabled{false};
  std::string model{"models/objects"};
  std::vector<std::string> classes;
  int inputSize{640};
  float confidence{0.45f};
  double maxFpsInference{2.0};
  double activeFps{6.0};
  double burstFps{10.0};
  int64_t burstMs{3000};
  bool useVulkan{true};
};

// [operator] — per-camera aggregation, cooldowns, zones and schedule.
struct OperatorConfig
{
  bool overlay{false};
  std::string overlayDir;
  int64_t aggregationWindowMs{5000};
  int64_t cooldownMs{30000};
  int nightStartHour{22};
  int nightEndHour{6};
  int presenceEscalationFrames{3};
  bool zonesFromDb{true};
  int64_t zonesRefreshMs{30000};
  int64_t cameraRescanMs{15000};
  bool motionGate{false};
  double motionMinRatio{0.002};
  bool ignoreStaticPersons{false};
  int staticBoxFrames{8};
  int64_t dwellAlertMs{3000};
  int64_t dwellMonitorMs{12000};
  int64_t dwellNightMs{8000};
  int64_t personRecheckMs{30000};
  int64_t trackTtlMs{3000};
  double trackIouMin{0.3};
  std::vector<std::string> ignoredClasses;
  std::vector<OperatorZone> zones;
};

// [identity] — person recognition through the identity RPC (fleet-gated).
struct IdentityConfig
{
  bool identify{false};
  bool autoEnroll{false};
  bool captureClearFaces{true};
  int minFaceBoxPx{48};
  int64_t identifyIntervalMs{2000};
  int64_t enrollCooldownMs{600000};
  int64_t bestShotMs{10000};
  double improveMargin{0.15};
  std::string target;
  std::string rpcSecret;
};

namespace operator_config
{
ObjectsConfig resolveObjects();
OperatorConfig resolveOperator();
IdentityConfig resolveIdentity();

// COCO-80, the default class table.
std::vector<std::string> defaultClasses();
} // namespace operator_config
