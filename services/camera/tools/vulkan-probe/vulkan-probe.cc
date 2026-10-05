#include <gpu.h>

#include <format>
#include <iostream>

int main()
{
  if (ncnn::create_gpu_instance() != 0) {
    std::cerr << "vulkan-probe: ncnn create_gpu_instance failed "
                 "(vkCreateInstance); detector stays on the CPU fallback\n";
    return 1;
  }

  const int count = ncnn::get_gpu_count();
  std::cout << std::format(
      "vulkan-probe: vkCreateInstance ok, physical devices = {}\n", count);
  for (int i = 0; i < count; ++i) {
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(i);
    std::cout << std::format(
        "vulkan-probe: device {}: {} / driver {} / api {:#x}\n", i,
        info.device_name(), info.driver_name(), info.api_version());
  }
  ncnn::destroy_gpu_instance();
  return count > 0 ? 0 : 1;
}
