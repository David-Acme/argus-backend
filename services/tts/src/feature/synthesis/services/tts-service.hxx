#pragma once

#include <tts/tts-wire.hxx>

#include <drogon/utils/coroutine.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class TtsEngine;
class UnicodeProcessor;
class Style;
class PocketEngine;
struct PocketVoice;

struct TtsStreamInput
{
  TtsRequest request;
  std::function<void(std::vector<float>)> onChunk;
  std::function<bool()> stopRequested;
};

enum class SpeechEngineKind : std::uint8_t
{
  Supertonic,
  Pocket
};

struct PocketSelection
{
  std::string variant;
  std::filesystem::path directory;
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

  [[nodiscard]] std::vector<std::pair<std::string, std::string>> activeEngines() const;

  static const std::vector<std::string>& supportedLangs();

  static int effectiveStepsCap();

  [[nodiscard]] static SpeechEngineKind configuredEngine(TtsLang lang);
  [[nodiscard]] static std::string engineName(SpeechEngineKind kind);
  [[nodiscard]] static const std::vector<std::string>& pocketLanguages();
  [[nodiscard]] static const std::vector<std::string>& pocketVoices(TtsLang lang);
  [[nodiscard]] static std::string configuredPocketVoice(TtsLang lang);
  [[nodiscard]] static std::string configuredPocketVariant(TtsLang lang);
  [[nodiscard]] static float configuredPocketTemperature();
  [[nodiscard]] static int configuredPocketLsdSteps();
  [[nodiscard]] static bool normalizationEnabled();
  [[nodiscard]] static std::filesystem::path pocketModelsDir();
  [[nodiscard]] static std::string speechText(const TtsRequest& req);

private:
  struct PocketStreamJob
  {
    const TtsRequest& request;
    const std::string& text;
    const std::function<void(std::vector<float>)>& onChunk;
    const std::function<bool()>& stopped;
  };

  struct VoiceRequest
  {
    PocketEngine& engine;
    const PocketSelection& selection;
    TtsLang lang{TtsLang::EN};
  };

  void loadDefaults();
  static int resolveSteps(TtsQuality quality);
  const Style& resolveVoice(const std::string& voiceId);
  static TtsQuality autoQuality(const std::string& text);
  TtsQuality resolveQuality(const TtsRequest& req) const;
  std::unique_lock<std::timed_mutex> acquire(const std::function<bool()>& stopped);
  bool streamPocket(const PocketStreamJob& job);
  [[nodiscard]] std::optional<PocketSelection> pocketSelection(TtsLang lang) const;
  PocketEngine* pocketEngine(const PocketSelection& selection);
  std::shared_ptr<const PocketVoice> pocketVoice(const VoiceRequest& request);
  std::shared_ptr<const PocketVoice> referenceVoice(const VoiceRequest& request);
  [[nodiscard]] std::string cacheKey(const TtsRequest& req) const;
  void remember(const std::string& key, const std::vector<float>& samples);
  void warnOnce(const std::string& message);

  std::unique_ptr<TtsEngine> engine_;
  std::unique_ptr<UnicodeProcessor> processor_;
  std::unordered_map<std::string, std::unique_ptr<Style>> voiceCache_;
  std::unordered_map<std::string, std::unique_ptr<PocketEngine>> pocketEngines_;
  std::unordered_map<std::string, std::shared_ptr<const PocketVoice>> pocketVoices_;
  std::unordered_set<std::string> pocketFailures_;
  std::unordered_set<std::string> warnings_;

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
  int threads_{1};
  bool loaded_ = false;
  std::atomic<bool> stopping_{true};
  std::atomic<std::uint64_t> generation_{0};
  std::mutex lifecycleMutex_;
  mutable std::timed_mutex synthMutex_;
};
