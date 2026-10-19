#include "keyword-spotter.hxx"

#include <sherpa-onnx/c-api/c-api.h>
#include <trantor/utils/Logger.h>

#include <filesystem>
#include <string_view>
#include <utility>

namespace
{

void destroySpotter(const SherpaOnnxKeywordSpotter* spotter)
{
  if (spotter != nullptr)
    SherpaOnnxDestroyKeywordSpotter(spotter);
}

void destroyStream(const SherpaOnnxOnlineStream* stream)
{
  if (stream != nullptr)
    SherpaOnnxDestroyOnlineStream(stream);
}

std::string inDir(const std::string& dir, const std::string& file)
{
  const std::filesystem::path path(file);
  if (path.is_absolute() || dir.empty())
    return file;
  return (std::filesystem::path(dir) / path).string();
}

class SherpaSpotSession final : public SpotSession
{
public:
  SherpaSpotSession(const SherpaOnnxKeywordSpotter* spotter, std::shared_ptr<std::mutex> decodeMutex)
      : spotter_(spotter),
        decodeMutex_(std::move(decodeMutex)),
        stream_(SherpaOnnxCreateKeywordStream(spotter), destroyStream)
  {
  }

  [[nodiscard]] bool valid() const { return stream_ != nullptr; }

  std::vector<SpotHit> push(std::span<const float> samples, int sampleRate) override
  {
    std::vector<SpotHit> hits;
    if (samples.empty())
      return hits;
    std::scoped_lock lock(*decodeMutex_);
    SherpaOnnxOnlineStreamAcceptWaveform(stream_.get(), sampleRate, samples.data(),
                                         static_cast<int32_t>(samples.size()));
    fed_ += samples.size();
    while (SherpaOnnxIsKeywordStreamReady(spotter_, stream_.get()) != 0) {
      SherpaOnnxDecodeKeywordStream(spotter_, stream_.get());
      const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(spotter_, stream_.get());
      if (result != nullptr && result->keyword != nullptr && std::string_view(result->keyword).size() > 0) {
        hits.push_back({.keyword = result->keyword, .atSample = fed_, .startTime = result->start_time});
        SherpaOnnxResetKeywordStream(spotter_, stream_.get());
      }
      if (result != nullptr)
        SherpaOnnxDestroyKeywordResult(result);
    }
    return hits;
  }

  void reset() override
  {
    std::scoped_lock lock(*decodeMutex_);
    SherpaOnnxResetKeywordStream(spotter_, stream_.get());
  }

private:
  const SherpaOnnxKeywordSpotter* spotter_;
  std::shared_ptr<std::mutex> decodeMutex_;
  std::unique_ptr<const SherpaOnnxOnlineStream, void (*)(const SherpaOnnxOnlineStream*)> stream_;
  uint64_t fed_{0};
};

}

KeywordSpotterService::KeywordSpotterService() : spotter_(nullptr, destroySpotter) {}

KeywordSpotterService::~KeywordSpotterService() = default;

bool KeywordSpotterService::init(const SttSpotConfig& config)
{
  const std::string encoder = inDir(config.modelDir, config.encoder);
  const std::string decoder = inDir(config.modelDir, config.decoder);
  const std::string joiner = inDir(config.modelDir, config.joiner);
  const std::string tokens = inDir(config.modelDir, config.tokens);
  const std::string keywords = inDir(config.modelDir, config.keywordsFile);
  for (const std::string& file : {encoder, decoder, joiner, tokens, keywords}) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
      LOG_INFO << "STT keyword spotter not loaded: " << file
               << " is missing; the watchman falls back to transcribing speech";
      return false;
    }
  }
  SherpaOnnxKeywordSpotterConfig sherpa{};
  sherpa.feat_config.sample_rate = 16000;
  sherpa.feat_config.feature_dim = 80;
  sherpa.model_config.transducer.encoder = encoder.c_str();
  sherpa.model_config.transducer.decoder = decoder.c_str();
  sherpa.model_config.transducer.joiner = joiner.c_str();
  sherpa.model_config.tokens = tokens.c_str();
  sherpa.model_config.provider = "cpu";
  sherpa.model_config.num_threads = config.threads;
  sherpa.model_config.debug = 0;
  sherpa.max_active_paths = config.maxActivePaths;
  sherpa.num_trailing_blanks = config.trailingBlanks;
  sherpa.keywords_score = config.score;
  sherpa.keywords_threshold = config.threshold;
  sherpa.keywords_file = keywords.c_str();
  spotter_.reset(SherpaOnnxCreateKeywordSpotter(&sherpa));
  loaded_.store(spotter_ != nullptr);
  if (spotter_)
    LOG_INFO << "STT keyword spotter loaded from " << config.modelDir << " (threshold " << config.threshold
             << ", score " << config.score << ")";
  else
    LOG_WARN << "STT keyword spotter failed to load from " << config.modelDir;
  return loaded_.load();
}

std::unique_ptr<SpotSession> KeywordSpotterService::open()
{
  if (!spotter_)
    return nullptr;
  auto session = std::make_unique<SherpaSpotSession>(spotter_.get(), decodeMutex_);
  if (!session->valid())
    return nullptr;
  return session;
}
