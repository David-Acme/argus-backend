#pragma once

#include <shared/services/tts/tts-wire.hxx>

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

  // Denoising-steps ceiling; tts.steps_cap pins it for extracted deployments.
  static int effectiveStepsCap();

private:
  static int resolveSteps(TtsQuality quality);
  const Style& resolveVoice(const std::string& voiceId);
  static TtsQuality autoQuality(const std::string& text);
  // Effective quality: request wins, then tts.quality, then autoQuality() by text length.
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
