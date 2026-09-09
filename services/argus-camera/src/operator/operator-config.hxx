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
  std::vector<std::string> ignoredClasses;
  std::vector<OperatorZone> zones;
};

namespace operator_config
{
ObjectsConfig resolveObjects();
OperatorConfig resolveOperator();

// COCO-80, the default class table.
std::vector<std::string> defaultClasses();
} // namespace operator_config
