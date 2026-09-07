#pragma once

#include "tts-wire.hxx"

#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <unordered_map>
#include <vector>

class TtsEngine;
class UnicodeProcessor;
class Style;

class TtsService
{
public:
  TtsService();
  ~TtsService();

  TtsService(const TtsService&) = delete;
  TtsService& operator=(const TtsService&) = delete;

  static TtsService& instance();

  void init();
  void shutdown();
  bool isLoaded() const;

  std::vector<float> synthesize(const TtsRequest& req);
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk);

  // Coroutine variants: run synthesis off the event loop.
  drogon::Task<std::vector<float>> synthesizeAsync(const TtsRequest& req);
  drogon::Task<void> synthesizeStreamAsync(const TtsRequest& req,
                                           TtsChunkCallback onChunk);
  void loadVoice(const std::string& voiceId);

  void setDefaultQuality(TtsQuality q);
  TtsQuality defaultQuality() const;

  // Configured base speech speed (config.toml [tts] speed).
  float defaultSpeed() const;

  int sampleRate() const;
  std::vector<std::string> availableVoices() const;

  static void writeWav(const std::string& path, const std::vector<float>& pcm,
                       int sampleRate = 44100);

  static const std::vector<std::string>& supportedLangs();

  // Denoising-steps ceiling: tier-derived, overridable with tts.steps_cap so
  // an extracted deployment (no Vulkan probe) can pin the legacy cap.
  static int effectiveStepsCap();

private:
  static int resolveSteps(TtsQuality quality);
  const Style& resolveVoice(const std::string& voiceId);
  static TtsQuality autoQuality(const std::string& text);
  // Resolves the effective quality: an explicit request quality wins; an
  // "Auto" request falls back to the configured default (tts.quality), which
  // in turn falls back to the adaptive autoQuality() by text length.
  TtsQuality resolveQuality(const TtsRequest& req) const;

  std::unique_ptr<TtsEngine> engine_;
  std::unique_ptr<UnicodeProcessor> processor_;
  std::unordered_map<std::string, std::unique_ptr<Style>> voiceCache_;
  Ort::Env env_{ORT_LOGGING_LEVEL_ERROR, "Argus-TTS"};
  TtsQuality defaultQuality_{TtsQuality::Auto};
  float defaultSpeed_{1.0F};
  int maxChunkLen_{300};
  bool loaded_ = false;
  mutable std::mutex synthMutex_;
  mutable std::mutex voiceMutex_;
};
