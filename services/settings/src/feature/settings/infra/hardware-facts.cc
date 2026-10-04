#include "hardware-facts.hxx"

#include <cmath>

namespace
{
constexpr double kMegabytesPerGigabyte = 1024.0;
constexpr double kTenths = 10.0;

CpuIsa isaOf(const HardwareProfile& profile)
{
  if (profile.avx512)
    return CpuIsa::Avx512;
  if (profile.avx2)
    return CpuIsa::Avx2;
  if (profile.neon)
    return CpuIsa::Neon;
  return CpuIsa::Baseline;
}
}

HardwareFacts hardwareFactsOf(const HardwareProfile& profile)
{
  const double gigabytes = static_cast<double>(profile.ramTotalMb) / kMegabytesPerGigabyte;
  return {.cores = profile.physicalCores,
          .threads = profile.logicalThreads,
          .ramGb = std::round(gigabytes * kTenths) / kTenths,
          .isa = isaOf(profile),
          .gpu = profile.videoAccel};
}

HardwareFacts probeHardwareFacts()
{
  return hardwareFactsOf(HardwareProbe::get());
}
