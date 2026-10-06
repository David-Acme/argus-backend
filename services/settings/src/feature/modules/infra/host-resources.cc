#include "host-resources.hxx"

#include <sys/statvfs.h>

std::optional<std::int64_t> freeDiskBytes(const std::string& directory)
{
  struct statvfs stats{};
  if (directory.empty() || ::statvfs(directory.c_str(), &stats) != 0)
    return std::nullopt;
  return static_cast<std::int64_t>(stats.f_bavail) * static_cast<std::int64_t>(stats.f_frsize);
}

HostResources hostResourcesOf(const HardwareProfile& profile)
{
  HostResources host{.ramTotalMb = profile.ramTotalMb, .freeDiskBytes = std::nullopt, .cpuFeatures = {}, .gpu = profile.vulkan};
  if (profile.avx2)
    host.cpuFeatures.emplace_back("avx2");
  if (profile.avx512)
    host.cpuFeatures.emplace_back("avx512");
  if (profile.fma)
    host.cpuFeatures.emplace_back("fma");
  if (profile.neon)
    host.cpuFeatures.emplace_back("neon");
  return host;
}
