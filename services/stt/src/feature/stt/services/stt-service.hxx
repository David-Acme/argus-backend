#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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

struct TranscribeRequest
{
  std::vector<float> samples;
  int32_t sampleRate{16000};
  std::string lang;
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

  static SttEngine configEngine();

  static std::string_view engineName(SttEngine engine);

  static bool isSupportedLanguage(const std::string& lang);

  static const std::vector<std::string>& supportedLanguages();

  void init();
  void shutdown();

  std::string transcribe(const TranscribeRequest& request);

  bool setLanguage(const std::string& lang);

  std::string language() const;

  drogon::Task<std::string> transcribeAsync(TranscribeRequest request);

  bool isLoaded() const;

private:
  std::string decode(const std::vector<float>& samples, int32_t sampleRate);

  std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                  void (*)(const SherpaOnnxOfflineRecognizer*)>
      recognizer_;
  std::string currentLang_;
  SttEngine engine_ = SttEngine::NemoTransducer;
  bool loaded_ = false;
  mutable std::mutex mutex_;
};
