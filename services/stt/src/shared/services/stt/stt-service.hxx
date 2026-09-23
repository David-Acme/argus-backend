#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SherpaOnnxOfflineRecognizer;

enum class SttEngine
{
  Whisper,
  Canary,
  NemoCtc,
  NemoTransducer,
  Omnilingual
};

struct TranscribeInput
{
  const std::vector<float>& audioSamples;
  int32_t sampleRate{16000};
  const std::string& lang;
};

class SttService
{
public:
  SttService();
  ~SttService();

  SttService(const SttService&) = delete;
  SttService& operator=(const SttService&) = delete;

  static SttService& instance();

  static std::string configLanguage();

  static bool isSupportedLanguage(const std::string& lang);

  void init();
  void shutdown();

  std::string transcribe(const std::vector<float>& audioSamples,
                         int32_t sampleRate = 16000);

  bool setLanguage(const std::string& lang);

  std::string language() const;

  drogon::Task<std::string> transcribeAsync(const TranscribeInput& input);

  bool isLoaded() const;

private:
  std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                  void (*)(const SherpaOnnxOfflineRecognizer*)>
      recognizer_;
  std::string currentLang_;
  bool loaded_ = false;
  mutable std::mutex mutex_;
};
