#pragma once

#include <feature/modules/schemas/module-catalog.hxx>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

enum class HardwareVerdict : std::uint8_t
{
  Ok,
  Slow,
  Insufficient
};

constexpr std::string_view hardwareVerdictToString(HardwareVerdict verdict)
{
  switch (verdict) {
  case HardwareVerdict::Ok: return "ok";
  case HardwareVerdict::Slow: return "slow";
  case HardwareVerdict::Insufficient: return "insufficient";
  }
  return "ok";
}

struct HostResources
{
  std::int64_t ramTotalMb{0};
  std::optional<std::int64_t> freeDiskBytes;
  std::vector<std::string> cpuFeatures;
  bool gpu{false};
};

struct HardwareCheckInput
{
  const ModuleCatalog& catalog;
  const CatalogModule& module;
  const std::set<std::string>& enabled;
  std::int64_t remainingBytes{0};
  const HostResources& host;
};

struct HardwareAssessment
{
  HardwareVerdict verdict{HardwareVerdict::Ok};
  std::vector<std::string> reasons;
  std::int64_t minRamMb{0};
  std::int64_t recommendedRamMb{0};
  std::optional<std::int64_t> freeDiskMb;
};

namespace hardware_reason
{
inline constexpr std::string_view kRamBelowMinimum = "ram_below_minimum";
inline constexpr std::string_view kRamBelowRecommended = "ram_below_recommended";
inline constexpr std::string_view kDiskInsufficient = "disk_insufficient";
inline constexpr std::string_view kCpuFeatureMissing = "cpu_feature_missing";
inline constexpr std::string_view kGpuMissing = "gpu_missing";
}

[[nodiscard]] HardwareAssessment assessHardware(const HardwareCheckInput& input);

[[nodiscard]] std::int64_t diskNeededBytes(std::int64_t remainingBytes);
