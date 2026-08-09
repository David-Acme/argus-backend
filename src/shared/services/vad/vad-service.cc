#include "vad-service.hxx"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared/services/config-service/config-service.hxx>

namespace
{

constexpr int kWindowSize = 512; // 32ms at 16 kHz
constexpr int kContextSize = 64; // Silero pads the window with 64 prev samples
constexpr int kEffectiveWindow = kWindowSize + kContextSize;
constexpr int kStateSize = 2 * 1 * 128; // [2, 1, 128]
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

} // namespace

VadService::VadService() : VadService(VadConfig{}) {}

VadService::VadService(const VadConfig& config)
    : cfg_(config), pending_(kWindowSize * 8), window_(kEffectiveWindow),
      sampleRateInput_{16000}, inputShape_{1, kEffectiveWindow},
      stateShape_{2, 1, 128}, srShape_{1}
{
  if (const double v = ConfigService::getDouble("vad.threshold"); v > 0.0)
    cfg_.threshold = static_cast<float>(v);
  if (const double v = ConfigService::getDouble("vad.neg_threshold"); v > 0.0)
    cfg_.negThreshold = static_cast<float>(v);
  if (const int v = ConfigService::getInt("vad.min_speech_frames"); v > 0)
    cfg_.minSpeechFrames = v;
  if (const int v = ConfigService::getInt("vad.min_silence_frames"); v > 0)
    cfg_.minSilenceFrames = v;
  if (const int v = ConfigService::getInt("vad.max_turn_frames"); v > 0)
    cfg_.maxTurnFrames = v;
  if (const int v = ConfigService::getInt("vad.pre_roll_frames"); v > 0)
    cfg_.preRollFrames = v;
  if (const int v = ConfigService::getInt("vad.min_turn_ms"); v > 0)
    cfg_.minTurnMs = v;
  if (const double v = ConfigService::getDouble("vad.min_mean_prob"); v > 0.0)
    cfg_.minMeanProb = static_cast<float>(v);
  reset();
}

VadService::~VadService() = default;

bool VadService::isLoaded()
{
  try {
    (void)vadSession();
    return true;
  }
  catch (...) {
    return false;
  }
}

void VadService::runModel(float& prob)
{
  auto input =
      Ort::Value::CreateTensor<float>(vadMem(), window_.data(), window_.size(),
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

  const char* inNames[] = {"input", "state", "sr"};
  const char* outNames[] = {"output", "stateN"};

  std::lock_guard<std::mutex> lock(vadSessionMutex());
  auto outs = vadSession().Run(Ort::RunOptions{}, inNames, feeds.data(),
                               feeds.size(), outNames, 2);
  prob = outs[0].GetTensorMutableData<float>()[0];
  std::memcpy(state_.data(), outs[1].GetTensorMutableData<float>(),
              kStateSize * sizeof(float));
}

bool VadService::process(const float* samples, int count, VadTurn& outTurn)
{
  outTurn.samples.clear();
  outTurn.speechFrames = 0;
  outTurn.meanProb = 0.0F;
  bool completed = false;

  pending_.push(samples, static_cast<size_t>(count));

  while (pending_.size() >= static_cast<size_t>(kWindowSize)) {
    std::copy(context_.begin(), context_.end(), window_.begin());
    pending_.pop(window_.data() + kContextSize, kWindowSize);
    std::copy(window_.begin() + kWindowSize - kContextSize,
              window_.begin() + kWindowSize, context_.begin());

    float prob = 0.0F;
    runModel(prob);
    lastProb_ = prob;

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

    if (!speech_) {
      preRoll_.insert(preRoll_.end(), window_.begin() + kContextSize,
                      window_.end());
      if (static_cast<int>(preRoll_.size()) >
          cfg_.preRollFrames * kWindowSize) {
        preRoll_.erase(preRoll_.begin(),
                       preRoll_.begin() + (static_cast<int>(preRoll_.size()) -
                                           cfg_.preRollFrames * kWindowSize));
      }
    }

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
      if (accepted) {
        outTurn.samples = std::move(buffer_);
        outTurn.speechFrames = frameCounter_;
        outTurn.meanProb = meanProb;
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
  return completed;
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
  state_.assign(kStateSize, 0.0F);
  context_.assign(kContextSize, 0.0F);
  pending_.clear();
  speech_ = false;
  startCounter_ = 0;
  silenceCounter_ = 0;
  frameCounter_ = 0;
  speechProbSum_ = 0.0F;
  buffer_.clear();
  preRoll_.clear();
  lastProb_ = 0.0F;
}
