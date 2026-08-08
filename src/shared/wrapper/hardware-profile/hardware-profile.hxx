#pragma once

#include <cstdint>
#include <string>

enum class VideoAccel
{
  None,
  Vaapi,
  Qsv,
  Nvdec,
  VideoToolbox
};

enum class CapabilityTier
{
  Minimal,
  Low,
  Balanced,
  High
};

const char* toString(VideoAccel accel);
const char* toString(CapabilityTier tier);

struct HardwareProfile
{
  int physicalCores{1};
  int logicalThreads{1};
  bool avx2{false};
  bool avx512{false};
  bool fma{false};
  bool neon{false};
  int64_t ramTotalMb{0};
  int64_t ramAvailableMb{0};

  bool vulkan{false};
  bool vulkanDiscrete{false};
  std::string vulkanDevice;
  int64_t vulkanVramMb{0};

  VideoAccel videoAccel{VideoAccel::None};
  std::string videoDevice;

  CapabilityTier tier{CapabilityTier::Minimal};
};

namespace HardwareProbe
{

const HardwareProfile& get();

std::string describe();

int detectorInputSize();
int analysisFps();
bool vlmEnabled();
bool toolsEnabled();
int llmGpuLayers();
int vlmGpuLayers();
int ortSpinDurationUs();
const char* llmKvType();
int ttsStepsCap();

} // namespace HardwareProbe
