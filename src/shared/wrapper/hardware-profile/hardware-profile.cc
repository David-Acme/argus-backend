#include "hardware-profile.hxx"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#if __has_include(<gpu.h>)
#include <gpu.h>
#define ARGUS_HAS_NCNN_GPU 1
#endif

namespace
{

int probeLogicalThreads()
{
  const auto hw = std::thread::hardware_concurrency();
  return static_cast<int>(std::max(1U, hw));
}

int probePhysicalCores(int logical)
{
  std::set<std::string> siblings;
  const std::filesystem::path base{"/sys/devices/system/cpu"};
  std::error_code ec;
  if (!std::filesystem::exists(base, ec))
    return logical;

  for (int i = 0; i < logical; ++i) {
    std::ostringstream path;
    path << "/sys/devices/system/cpu/cpu" << i
         << "/topology/thread_siblings_list";
    std::ifstream f(path.str());
    if (!f.is_open())
      continue;
    std::string line;
    std::getline(f, line);
    if (!line.empty())
      siblings.insert(line);
  }
  if (siblings.empty())
    return logical;
  return static_cast<int>(siblings.size());
}

int64_t probeRamTotalMb()
{
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || pageSize <= 0)
    return 0;
  return (static_cast<int64_t>(pages) * pageSize) / (1024 * 1024);
}

int64_t probeRamAvailableMb()
{
  std::ifstream f("/proc/meminfo");
  std::string key;
  int64_t value = 0;
  std::string unit;
  while (f >> key >> value >> unit) {
    if (key == "MemAvailable:")
      return value / 1024;
  }
  return 0;
}

std::string drmDriver(const std::string& node)
{
  std::ifstream f("/sys/class/drm/" + node + "/device/uevent");
  std::string line;
  while (std::getline(f, line)) {
    const auto pos = line.find("DRIVER=");
    if (pos == 0)
      return line.substr(7);
  }
  return {};
}

void probeVideoAccel(HardwareProfile& p)
{
  std::error_code ec;
  const std::filesystem::path dri{"/dev/dri"};
  if (!std::filesystem::exists(dri, ec))
    return;

  for (const auto& entry : std::filesystem::directory_iterator(dri, ec)) {
    const auto name = entry.path().filename().string();
    if (name.rfind("renderD", 0) != 0)
      continue;
    const auto driver = drmDriver(name);
    if (driver == "nvidia" || driver == "nvidia-drm") {
      p.videoAccel = VideoAccel::Nvdec;
      p.videoDevice = entry.path().string();
      return;
    }
    if (driver == "i915" || driver == "xe") {
      p.videoAccel = VideoAccel::Qsv;
      p.videoDevice = entry.path().string();
      return;
    }
    if (driver == "amdgpu" || driver == "radeon") {
      p.videoAccel = VideoAccel::Vaapi;
      p.videoDevice = entry.path().string();
      return;
    }
    if (p.videoAccel == VideoAccel::None) {
      p.videoAccel = VideoAccel::Vaapi;
      p.videoDevice = entry.path().string();
    }
  }
}

void probeVulkan(HardwareProfile& p)
{
#ifdef ARGUS_HAS_NCNN_GPU
  const int count = ncnn::get_gpu_count();
  if (count <= 0)
    return;

  const auto& info = ncnn::get_gpu_info(0);
  p.vulkan = true;
  p.vulkanDevice = info.device_name() ? info.device_name() : "unknown";
  p.vulkanDiscrete = info.type() == 0;

  int64_t best = 0;
  for (uint32_t h = 0; h < info.physicalDeviceMemoryProperties().memoryHeapCount; ++h) {
    const auto& heap = info.physicalDeviceMemoryProperties().memoryHeaps[h];
    if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
      best = std::max(best, static_cast<int64_t>(heap.size / (1024 * 1024)));
  }
  p.vulkanVramMb = best;
#else
  (void)p;
#endif
}

void probeCpuFeatures(HardwareProfile& p)
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_cpu_init();
  p.avx2 = __builtin_cpu_supports("avx2");
  p.avx512 = __builtin_cpu_supports("avx512f");
  p.fma = __builtin_cpu_supports("fma");
#elif defined(__aarch64__)
  p.neon = true;
#endif
}

CapabilityTier deriveTier(const HardwareProfile& p)
{
  if (p.physicalCores <= 2 || p.ramTotalMb < 4096)
    return CapabilityTier::Minimal;

  if (p.vulkan && p.vulkanDiscrete && p.vulkanVramMb >= 4096 &&
      p.physicalCores >= 8)
    return CapabilityTier::High;

  if (p.vulkan && p.physicalCores >= 6 && p.ramTotalMb >= 8192)
    return CapabilityTier::Balanced;

  return CapabilityTier::Low;
}

HardwareProfile probeAll()
{
  HardwareProfile p;
  p.logicalThreads = probeLogicalThreads();
  p.physicalCores = probePhysicalCores(p.logicalThreads);
  p.ramTotalMb = probeRamTotalMb();
  p.ramAvailableMb = probeRamAvailableMb();
  probeCpuFeatures(p);
  probeVulkan(p);
  probeVideoAccel(p);
  p.tier = deriveTier(p);
  return p;
}

} // namespace

const char* toString(VideoAccel accel)
{
  switch (accel) {
  case VideoAccel::Vaapi:
    return "vaapi";
  case VideoAccel::Qsv:
    return "qsv";
  case VideoAccel::Nvdec:
    return "nvdec";
  case VideoAccel::VideoToolbox:
    return "videotoolbox";
  case VideoAccel::None:
  default:
    return "none";
  }
}

const char* toString(CapabilityTier tier)
{
  switch (tier) {
  case CapabilityTier::Low:
    return "low";
  case CapabilityTier::Balanced:
    return "balanced";
  case CapabilityTier::High:
    return "high";
  case CapabilityTier::Minimal:
  default:
    return "minimal";
  }
}

namespace HardwareProbe
{

const HardwareProfile& get()
{
  static const HardwareProfile profile = probeAll();
  return profile;
}

std::string describe()
{
  const auto& p = get();
  std::ostringstream os;
  os << "tier=" << toString(p.tier) << " cores=" << p.physicalCores << "p/"
     << p.logicalThreads << "l"
     << " ram=" << p.ramTotalMb << "MB(avail " << p.ramAvailableMb << ")"
     << " isa=";
  if (p.avx512)
    os << "avx512 ";
  else if (p.avx2)
    os << "avx2 ";
  else if (p.neon)
    os << "neon ";
  else
    os << "baseline ";
  os << "vulkan=";
  if (p.vulkan)
    os << (p.vulkanDiscrete ? "discrete:" : "integrated:") << p.vulkanDevice
       << "(" << p.vulkanVramMb << "MB)";
  else
    os << "no";
  os << " video=" << toString(p.videoAccel);
  if (!p.videoDevice.empty())
    os << ":" << p.videoDevice;
  return os.str();
}

int detectorInputSize()
{
  return get().tier == CapabilityTier::High ? 640 : 512;
}

int analysisFps()
{
  switch (get().tier) {
  case CapabilityTier::High:
    return 12;
  case CapabilityTier::Balanced:
    return 8;
  case CapabilityTier::Low:
    return 4;
  case CapabilityTier::Minimal:
  default:
    return 2;
  }
}

bool vlmEnabled()
{
  return get().tier != CapabilityTier::Minimal;
}

bool toolsEnabled()
{
  return get().tier != CapabilityTier::Minimal;
}

int llmGpuLayers()
{
  const auto& p = get();
  if (!p.vulkan)
    return 0;
  if (p.ramTotalMb < 8192)
    return 0;
  return 999;
}

int vlmGpuLayers()
{
  return llmGpuLayers();
}

int ortSpinDurationUs()
{
  return get().physicalCores >= 8 ? 1000 : 0;
}

const char* llmKvType()
{
  const auto& p = get();
  if (p.ramTotalMb >= 16384)
    return "f16";
  if (p.ramTotalMb >= 8192)
    return "q8_0";
  return "q4_0";
}

int ttsStepsCap()
{
  switch (get().tier) {
  case CapabilityTier::High:
    return 16;
  case CapabilityTier::Balanced:
    return 12;
  case CapabilityTier::Low:
    return 8;
  case CapabilityTier::Minimal:
  default:
    return 5;
  }
}

} // namespace HardwareProbe
