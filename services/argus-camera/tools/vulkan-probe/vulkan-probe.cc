#include <gpu.h>

#include <cstdio>

int main()
{
  if (ncnn::create_gpu_instance() != 0) {
    std::fprintf(stderr,
                 "vulkan-probe: ncnn create_gpu_instance failed "
                 "(vkCreateInstance); detector stays on the CPU fallback\n");
    return 1;
  }

  const int count = ncnn::get_gpu_count();
  std::printf("vulkan-probe: vkCreateInstance ok, physical devices = %d\n",
              count);
  for (int i = 0; i < count; ++i) {
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(i);
    std::printf("vulkan-probe: device %d: %s / driver %s / api 0x%x\n", i,
                info.device_name(), info.driver_name(), info.api_version());
  }
  ncnn::destroy_gpu_instance();
  return count > 0 ? 0 : 1;
}
