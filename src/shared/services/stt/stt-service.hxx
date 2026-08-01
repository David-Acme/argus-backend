#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SherpaOnnxOfflineRecognizer;

class SttService
{
public:
  SttService() = delete;
  ~SttService() = delete;

  static void init();
  static void shutdown();

  static std::string transcribe(const std::vector<float>& audioSamples,
                                int32_t sampleRate = 16000);

  // Coroutine variant: runs inference off the event loop.
  static drogon::Task<std::string>
  transcribeAsync(const std::vector<float>& audioSamples,
                  int32_t sampleRate = 16000);

  static bool isLoaded();

private:
  static std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                         void (*)(const SherpaOnnxOfflineRecognizer*)>
      recognizer_;
  static bool loaded_;
  static std::mutex mutex_;
};
