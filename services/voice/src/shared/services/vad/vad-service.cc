#include "vad-service.hxx"

#include <drogon/drogon.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <config/config-service.hxx>

namespace
{

constexpr int kWindowSize = 512;
constexpr int kContextSize = 64;
constexpr int kEffectiveWindow = kWindowSize + kContextSize;
constexpr int kStateSize = 2 * 1 * 128;
constexpr const char* kModelPath = "models/vad/silero_vad.onnx";

Ort::MemoryInfo& vadMem()
{
  static Ort::MemoryInfo info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return info;
}

Ort::Session& vadSession()
{
  static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "Argus-Vad");
  static Ort::Session session = [&] {
    auto opts = Ort::SessionOptions{};
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(1);
    opts.SetInterOpNumThreads(1);
    return Ort::Session(env, kModelPath, opts);
  }();
  return session;
}

std::mutex& vadSessionMutex()
{
  static std::mutex mutex;
  return mutex;
}

class SileroVadModel final : public VadModel
{
public:
  SileroVadModel()
      : input_(kEffectiveWindow), sampleRateInput_{16000},
        inputShape_{1, kEffectiveWindow}, stateShape_{2, 1, 128}, srShape_{1}
  {
    SileroVadModel::reset();
  }

  float probability(std::span<const float> window) override
  {
    std::ranges::copy(window.first(std::min(window.size(), input_.size())),
                      input_.begin());
    auto input =
        Ort::Value::CreateTensor<float>(vadMem(), input_.data(), input_.size(),
                                        inputShape_.data(), inputShape_.size());
    auto state =
        Ort::Value::CreateTensor<float>(vadMem(), state_.data(), state_.size(),
                                        stateShape_.data(), stateShape_.size());
    auto srVal =
        Ort::Value::CreateTensor<int64_t>(vadMem(), sampleRateInput_.data(),
                                          sampleRateInput_.size(),
                                          srShape_.data(), srShape_.size());

    std::vector<Ort::Value> feeds;
    feeds.push_back(std::move(input));
    feeds.push_back(std::move(state));
    feeds.push_back(std::move(srVal));

    constexpr std::array<const char*, 3> inNames{"input", "state", "sr"};
    constexpr std::array<const char*, 2> outNames{"output", "stateN"};

    std::scoped_lock lock(vadSessionMutex());
    auto outs = vadSession().Run(Ort::RunOptions{}, inNames.data(), feeds.data(),
                                 feeds.size(), outNames.data(), outNames.size());
    const float prob = outs[0].GetTensorMutableData<float>()[0];
    std::memcpy(state_.data(), outs[1].GetTensorMutableData<float>(),
                static_cast<size_t>(kStateSize) * sizeof(float));
    return prob;
  }

  void reset() override { state_.assign(kStateSize, 0.0F); }

private:
  std::vector<float> input_;
  std::vector<float> state_;
  std::array<int64_t, 1> sampleRateInput_;
  std::array<int64_t, 2> inputShape_;
  std::array<int64_t, 3> stateShape_;
  std::array<int64_t, 1> srShape_;
};

}

std::unique_ptr<VadModel> makeSileroVadModel()
{
  return std::make_unique<SileroVadModel>();
}

VadService::VadService() : VadService(makeSileroVadModel()) {}

VadConfig resolveVadConfig()
{
  VadConfig cfg;
  if (const double v = ConfigService::getDouble("vad.threshold"); v > 0.0)
    cfg.threshold = static_cast<float>(v);
  if (const double v = ConfigService::getDouble("vad.neg_threshold"); v > 0.0)
    cfg.negThreshold = static_cast<float>(v);
  if (const int v = ConfigService::getInt("vad.min_speech_frames"); v > 0)
    cfg.minSpeechFrames = v;
  if (const int v = ConfigService::getInt("vad.min_silence_frames"); v > 0)
    cfg.minSilenceFrames = v;
  if (const int v = ConfigService::getInt("vad.max_turn_frames"); v > 0)
    cfg.maxTurnFrames = v;
  if (const int v = ConfigService::getInt("vad.pre_roll_frames"); v > 0)
    cfg.preRollFrames = v;
  if (const int v = ConfigService::getInt("vad.min_turn_ms"); v > 0)
    cfg.minTurnMs = v;
  if (const double v = ConfigService::getDouble("vad.min_mean_prob"); v > 0.0)
    cfg.minMeanProb = static_cast<float>(v);
  if (const double v = ConfigService::getDouble("vad.barge_threshold"); v > 0.0)
    cfg.bargeThreshold = static_cast<float>(v);
  if (const int v = ConfigService::getInt("vad.barge_min_frames"); v > 0)
    cfg.bargeMinFrames = v;
  return cfg;
}

VadService::VadService(std::unique_ptr<VadModel> model)
    : cfg_(resolveVadConfig()), model_(std::move(model)),
      pending_(static_cast<size_t>(kWindowSize) * 8), window_(kEffectiveWindow)
{
  reset();
}

VadService::~VadService() = default;

bool VadService::isLoaded()
{
  try {
    static_cast<void>(vadSession());
    return true;
  }
  catch (...) {
    return false;
  }
}

float VadService::nextWindow()
{
  std::ranges::copy(context_, window_.begin());
  pending_.pop(window_.data() + kContextSize, kWindowSize);
  std::copy(window_.end() - kContextSize, window_.end(), context_.begin());
  lastProb_ = model_->probability(window_);
  return lastProb_;
}

void VadService::keepPreRoll(int frames)
{
  preRoll_.insert(preRoll_.end(), window_.begin() + kContextSize, window_.end());
  const auto limit = static_cast<size_t>(frames) * kWindowSize;
  if (preRoll_.size() > limit)
    preRoll_.erase(preRoll_.begin(),
                   preRoll_.begin() +
                       static_cast<std::ptrdiff_t>(preRoll_.size() - limit));
}

std::optional<VadTurn> VadService::process(const VadProcessInput& input)
{
  VadTurn turn;
  bool completed = false;

  pending_.push(input.samples, static_cast<size_t>(input.count));

  while (pending_.size() >= static_cast<size_t>(kWindowSize)) {
    const float prob = nextWindow();

    if (prob >= cfg_.threshold) {
      startCounter_++;
      silenceCounter_ = 0;
      if (!speech_ && startCounter_ >= cfg_.minSpeechFrames) {
        speech_ = true;
        buffer_ = preRoll_;
      }
    }
    else {
      startCounter_ = 0;
      if (speech_) {
        if (prob < cfg_.negThreshold)
          silenceCounter_++;
        else
          silenceCounter_ = 0;
      }
    }

    if (!speech_)
      keepPreRoll(cfg_.preRollFrames);

    if (speech_) {
      buffer_.insert(buffer_.end(), window_.begin() + kContextSize,
                     window_.end());
      speechProbSum_ += prob;
      frameCounter_++;
    }

    if (speech_ && (silenceCounter_ >= cfg_.minSilenceFrames ||
                    frameCounter_ >= cfg_.maxTurnFrames)) {
      const float meanProb =
          frameCounter_ > 0 ? speechProbSum_ / static_cast<float>(frameCounter_)
                            : 0.0F;
      const int speechMs = frameCounter_ * kWindowSize * 1000 / cfg_.sampleRate;
      const bool accepted =
          speechMs >= cfg_.minTurnMs && meanProb >= cfg_.minMeanProb;
      if (!accepted) {
        LOG_DEBUG << "turn discarded: speech=" << speechMs << "ms meanProb="
                  << meanProb << " (min " << cfg_.minTurnMs << "ms / "
                  << cfg_.minMeanProb << ")";
      }
      if (accepted) {
        turn.samples = std::move(buffer_);
        turn.speechFrames = frameCounter_;
        turn.meanProb = meanProb;
        completed = true;
      }
      buffer_.clear();
      preRoll_.clear();
      speech_ = false;
      startCounter_ = 0;
      silenceCounter_ = 0;
      frameCounter_ = 0;
      speechProbSum_ = 0.0F;
    }
  }
  if (!completed)
    return std::nullopt;
  return turn;
}

bool VadService::listen(const VadListenInput& input)
{
  pending_.push(input.samples, static_cast<size_t>(input.count));

  while (pending_.size() >= static_cast<size_t>(kWindowSize)) {
    const float prob = nextWindow();
    keepPreRoll(cfg_.preRollFrames + cfg_.bargeMinFrames);

    if (input.armed && prob >= cfg_.bargeThreshold) {
      ++bargeCounter_;
      bargeProbSum_ += prob;
    }
    else {
      bargeCounter_ = 0;
      bargeProbSum_ = 0.0F;
    }

    if (bargeCounter_ >= cfg_.bargeMinFrames) {
      speech_ = true;
      buffer_ = std::move(preRoll_);
      preRoll_.clear();
      startCounter_ = bargeCounter_;
      silenceCounter_ = 0;
      frameCounter_ = bargeCounter_;
      speechProbSum_ = bargeProbSum_;
      bargeCounter_ = 0;
      bargeProbSum_ = 0.0F;
      return true;
    }
  }
  return false;
}

bool VadService::inSpeech() const
{
  return speech_;
}

float VadService::lastProb() const
{
  return lastProb_;
}

void VadService::reset()
{
  model_->reset();
  context_.assign(kContextSize, 0.0F);
  pending_.clear();
  speech_ = false;
  startCounter_ = 0;
  silenceCounter_ = 0;
  frameCounter_ = 0;
  speechProbSum_ = 0.0F;
  buffer_.clear();
  preRoll_.clear();
  bargeCounter_ = 0;
  bargeProbSum_ = 0.0F;
  lastProb_ = 0.0F;
}
