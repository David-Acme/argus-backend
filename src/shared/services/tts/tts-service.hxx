#pragma once

#include "onnx-utils.hxx"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <onnxruntime_cxx_api.h>

class TtsEngine;
class UnicodeProcessor;
class Style;

enum class TtsQuality { Auto, Low, Medium, High };

struct TtsRequest
{
  std::string text;
  TtsLang lang{TtsLang::EN};
  std::string voiceId{"M3"};
  TtsQuality quality{TtsQuality::Auto};
  float speed{1.05f};
};

using TtsChunkCallback =
    std::function<void(const std::vector<float>& chunkPcm)>;

class TtsService
{
public:
  TtsService() = delete;
  ~TtsService() = delete;

  static void init();
  static void shutdown();

  static std::vector<float> synthesize(const TtsRequest& req);
  static void synthesizeStream(const TtsRequest& req,
                               TtsChunkCallback onChunk);
  static void loadVoice(const std::string& voiceId);

  static void setDefaultQuality(TtsQuality q);
  static TtsQuality defaultQuality();

  static int sampleRate();
  static std::vector<std::string> availableVoices();

  static void writeWav(const std::string& path,
                       const std::vector<float>& pcm,
                       int sampleRate = 44100);

  static const std::vector<std::string>& supportedLangs();

private:
  static int resolveSteps(TtsQuality quality);
  static const Style& resolveVoice(const std::string& voiceId);
  static TtsQuality autoQuality(const std::string& text);

  static std::unique_ptr<TtsEngine> engine_;
  static std::unique_ptr<UnicodeProcessor> processor_;
  static std::unordered_map<std::string, std::unique_ptr<Style>> voiceCache_;
  static Ort::Env env_;
  static TtsQuality defaultQuality_;
};
