#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SherpaOnnxOfflineRecognizer;

// STT engine backends supported by sherpa-onnx.
enum class SttEngine
{
  Whisper,        // whisper tiny/base/small (auto language)
  Canary,         // NeMo Canary 180m flash (en+es+de+fr)
  NemoCtc,        // NeMo FastConformer CTC (single-pass, very fast)
  NemoTransducer, // NeMo FastConformer transducer (RNN-T, better English)
  Omnilingual     // 1600-language CTC model
};

class SttService
{
public:
  SttService() = delete;
  ~SttService() = delete;

  static void init();
  static void shutdown();

  static std::string transcribe(const std::vector<float>& audioSamples,
                                int32_t sampleRate = 16000);

  // Recreates the recognizer with a different Whisper language code
  // (e.g. "es", "en"). Used by voice interfaces that switch language at
  // runtime. Returns false if the language is unsupported.
  static bool setLanguage(const std::string& lang);

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
