#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct mtmd_context;

struct VisionRequest
{
  std::vector<unsigned char> imageRgb;
  uint32_t width{0};
  uint32_t height{0};
  std::string prompt{"Describe this image."};
  int32_t maxTokens{4096};
  float temperature{0.7f};
};

class VisionService
{
public:
  VisionService() = delete;
  ~VisionService() = delete;

  static void init();
  static void shutdown();

  static std::string describe(const VisionRequest& req);

  static bool isLoaded();

private:
  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;
  static int64_t contextSize_;
  static bool loaded_;
};
