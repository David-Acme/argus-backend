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

  // Default language for a fresh recognizer (stt.language, Spanish fallback).
  static std::string configLanguage();

  // The language codes setLanguage accepts ("es", "en", "auto").
  static bool isSupportedLanguage(const std::string& lang);

  void init();
  void shutdown();

  std::string transcribe(const std::vector<float>& audioSamples,
                         int32_t sampleRate = 16000);

  // Recreates the recognizer with a different language code; false when unsupported.
  bool setLanguage(const std::string& lang);

  // Language the current recognizer was built with ("" when not loaded).
  std::string language() const;

  // Coroutine variant: runs inference off the event loop; a language change rebuilds there.
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
