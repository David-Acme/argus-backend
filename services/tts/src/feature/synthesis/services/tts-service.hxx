#pragma once

#include <tts/tts-wire.hxx>

#include <drogon/utils/coroutine.h>
#include <atomic>
#include <cstdint>
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

struct TtsStreamInput
{
  TtsRequest request;
  std::function<void(std::vector<float>)> onChunk;
  std::function<bool()> stopRequested;
};

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
  void synthesizeStream(TtsStreamInput input);

  drogon::Task<std::vector<float>> synthesizeAsync(const TtsRequest& req);
  drogon::Task<void> synthesizeStreamAsync(const TtsRequest& req,
                                           TtsChunkCallback onChunk);
  void loadVoice(const std::string& voiceId);

  void setDefaultQuality(TtsQuality q);
  TtsQuality defaultQuality() const;

  float defaultSpeed() const;
  void refreshDefaults();

  int sampleRate() const;
  std::vector<std::string> availableVoices() const;

  static const std::vector<std::string>& supportedLangs();

  static int effectiveStepsCap();

private:
  void loadDefaults();
  static int resolveSteps(TtsQuality quality);
  const Style& resolveVoice(const std::string& voiceId);
  static TtsQuality autoQuality(const std::string& text);
  TtsQuality resolveQuality(const TtsRequest& req) const;

  std::unique_ptr<TtsEngine> engine_;
  std::unique_ptr<UnicodeProcessor> processor_;
  std::unordered_map<std::string, std::unique_ptr<Style>> voiceCache_;

  struct CachedAudio
  {
    std::string key;
    std::vector<float> samples;
  };
  std::vector<CachedAudio> synthCache_;
  size_t synthCacheNext_{0};
  Ort::Env env_{ORT_LOGGING_LEVEL_ERROR, "Argus-TTS"};
  TtsQuality defaultQuality_{TtsQuality::Auto};
  float defaultSpeed_{1.0F};
  int maxChunkLen_{300};
  bool loaded_ = false;
  std::atomic<bool> stopping_{true};
  std::atomic<std::uint64_t> generation_{0};
  std::mutex lifecycleMutex_;
  mutable std::timed_mutex synthMutex_;
};
