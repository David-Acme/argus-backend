#include "hardware-check.hxx"

#include <feature/modules/services/module-resolver.hxx>

#include <algorithm>

namespace
{
constexpr std::int64_t kBytesPerMb = std::int64_t{1024} * 1024;
constexpr std::int64_t kDiskMarginPercent = 10;
constexpr std::int64_t kPercent = 100;

void note(HardwareAssessment& assessment, HardwareVerdict verdict, std::string_view reason)
{
  assessment.verdict = std::max(assessment.verdict, verdict);
  if (std::ranges::find(assessment.reasons, reason) == assessment.reasons.end())
    assessment.reasons.emplace_back(reason);
}

bool holds(const HostResources& host, const std::string& feature)
{
  return std::ranges::find(host.cpuFeatures, feature) != host.cpuFeatures.end();
}
}

std::int64_t diskNeededBytes(std::int64_t remainingBytes)
{
  const std::int64_t remaining = std::max<std::int64_t>(remainingBytes, 0);
  return remaining + (remaining * kDiskMarginPercent + kPercent - 1) / kPercent;
}

HardwareAssessment assessHardware(const HardwareCheckInput& input)
{
  HardwareAssessment assessment{.verdict = HardwareVerdict::Ok,
                                .reasons = {},
                                .minRamMb = input.module.hardware.minRamMb,
                                .recommendedRamMb = input.module.hardware.recommendedRamMb,
                                .freeDiskMb = std::nullopt};

  std::set<std::string> counted = input.enabled;
  for (const auto& id : module_resolver::installOrder(input.catalog, input.module.id))
    counted.insert(id);
  std::int64_t minimum = 0;
  std::int64_t recommended = 0;
  for (const auto& id : counted)
    if (const auto* module = input.catalog.module(id)) {
      minimum += module->hardware.minRamMb;
      recommended += module->hardware.recommendedRamMb;
    }
  if (input.host.ramTotalMb < minimum)
    note(assessment, HardwareVerdict::Insufficient, hardware_reason::kRamBelowMinimum);
  else if (input.host.ramTotalMb < recommended)
    note(assessment, HardwareVerdict::Slow, hardware_reason::kRamBelowRecommended);

  if (input.host.freeDiskBytes) {
    assessment.freeDiskMb = *input.host.freeDiskBytes / kBytesPerMb;
    if (*input.host.freeDiskBytes < diskNeededBytes(input.remainingBytes))
      note(assessment, HardwareVerdict::Insufficient, hardware_reason::kDiskInsufficient);
  }

  for (const auto& feature : input.module.hardware.requiredCpu)
    if (!holds(input.host, feature))
      note(assessment, HardwareVerdict::Insufficient, hardware_reason::kCpuFeatureMissing);
  for (const auto& feature : input.module.hardware.recommendedCpu)
    if (!holds(input.host, feature))
      note(assessment, HardwareVerdict::Slow, hardware_reason::kCpuFeatureMissing);
  if (input.module.hardware.recommendedGpu && !input.host.gpu)
    note(assessment, HardwareVerdict::Slow, hardware_reason::kGpuMissing);
  return assessment;
}
