#include "vad.hxx"

#include <algorithm>
#include <cstring>

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

} // namespace

Vad::Vad()
    : cfg_(Config{}), env_(ORT_LOGGING_LEVEL_ERROR, "Argus-Vad"),
      pending_(kWindowSize * 8), window_(kEffectiveWindow),
      sampleRateInput_{16000}, inputShape_{1, kEffectiveWindow},
      stateShape_{2, 1, 128}, srShape_{1}
{
  auto opts = Ort::SessionOptions{};
  opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  opts.SetIntraOpNumThreads(1);
  opts.SetInterOpNumThreads(1);
  session_ = std::make_unique<Ort::Session>(env_, kModelPath, opts);
  reset();
}

Vad::~Vad() = default;

void Vad::runModel(float& prob)
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

  auto outs = session_->Run(Ort::RunOptions{}, inNames, feeds.data(),
                            feeds.size(), outNames, 2);
  prob = outs[0].GetTensorMutableData<float>()[0];
  std::memcpy(state_.data(), outs[1].GetTensorMutableData<float>(),
              kStateSize * sizeof(float));
}

bool Vad::process(const float* samples, int count, std::vector<float>& outTurn)
{
  outTurn.clear();
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
      frameCounter_++;
    }

    if (speech_ && (silenceCounter_ >= cfg_.minSilenceFrames ||
                    frameCounter_ >= cfg_.maxTurnFrames)) {
      outTurn = std::move(buffer_);
      buffer_.clear();
      preRoll_.clear();
      speech_ = false;
      startCounter_ = 0;
      silenceCounter_ = 0;
      frameCounter_ = 0;
      completed = true;
    }
  }
  return completed;
}

bool Vad::inSpeech() const
{
  return speech_;
}

float Vad::lastProb() const
{
  return lastProb_;
}

void Vad::reset()
{
  state_.assign(kStateSize, 0.0F);
  context_.assign(kContextSize, 0.0F);
  pending_.clear();
  speech_ = false;
  startCounter_ = 0;
  silenceCounter_ = 0;
  frameCounter_ = 0;
  buffer_.clear();
  preRoll_.clear();
  lastProb_ = 0.0F;
}
