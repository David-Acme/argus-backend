#include "vad.hxx"

#include <algorithm>
#include <cstring>

namespace
{

constexpr int kWindowSize = 512;  // 32ms at 16 kHz
constexpr int kContextSize = 64;  // Silero pads the window with 64 prev samples
constexpr int kEffectiveWindow = kWindowSize + kContextSize;
constexpr int kStateSize = 2 * 1 * 128;  // [2, 1, 128]
constexpr const char* kModelPath = "models/vad/silero_vad.onnx";

Ort::MemoryInfo& vadMem()
{
  static Ort::MemoryInfo info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return info;
}

} // namespace

Vad::Vad() : cfg_(Config{}), env_(ORT_LOGGING_LEVEL_ERROR, "Argus-Vad")
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

void Vad::runModel(const float* window, int windowLen, float& prob)
{
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<int64_t> sr = {16000};
  const std::vector<int64_t> inputShape = {1, windowLen};
  const std::vector<int64_t> stateShape = {2, 1, 128};
  const std::vector<int64_t> srShape = {1};

  auto input = Ort::Value::CreateTensor<float>(
      vadMem(), const_cast<float*>(window), static_cast<size_t>(windowLen),
      inputShape.data(), inputShape.size());
  auto state = Ort::Value::CreateTensor<float>(
      vadMem(), state_.data(), state_.size(), stateShape.data(),
      stateShape.size());
  auto srVal = Ort::Value::CreateTensor<int64_t>(
      vadMem(), sr.data(), sr.size(), srShape.data(), srShape.size());

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

  // Buffer the samples so we can build full 512-sample windows.
  pending_.insert(pending_.end(), samples, samples + count);

  while (static_cast<int>(pending_.size()) >= kWindowSize) {
    std::vector<float> window(kEffectiveWindow);
    // [prev context | current window]
    std::copy(context_.begin(), context_.end(), window.begin());
    std::copy(pending_.begin(), pending_.begin() + kWindowSize,
              window.begin() + kContextSize);
    // Keep the last 64 samples as context for the next window.
    std::copy(pending_.end() - kContextSize, pending_.end(), context_.begin());
    pending_.erase(pending_.begin(), pending_.begin() + kWindowSize);

    float prob = 0.0F;
    runModel(window.data(), kEffectiveWindow, prob);

    if (prob >= cfg_.threshold) {
      startCounter_++;
      silenceCounter_ = 0;
      if (!speech_ && startCounter_ >= cfg_.minSpeechFrames) {
        speech_ = true;
        hasSpeech_ = true;
        // Prepend the retained pre-roll so the leading phoneme is not cut.
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
      preRoll_.insert(preRoll_.end(), window.begin() + kContextSize,
                      window.end());
      if (static_cast<int>(preRoll_.size()) >
          cfg_.preRollFrames * kWindowSize) {
        preRoll_.erase(preRoll_.begin(),
                       preRoll_.begin() +
                           (static_cast<int>(preRoll_.size()) -
                            cfg_.preRollFrames * kWindowSize));
      }
    }

    if (speech_) {
      buffer_.insert(buffer_.end(), window.begin() + kContextSize, window.end());
      frameCounter_++;
    }

    if (speech_ && (silenceCounter_ >= cfg_.minSilenceFrames ||
                    frameCounter_ >= cfg_.maxTurnFrames)) {
      outTurn = std::move(buffer_);
      buffer_.clear();
      preRoll_.clear();
      speech_ = false;
      hasSpeech_ = false;
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

void Vad::reset()
{
  state_.assign(kStateSize, 0.0F);
  context_.assign(kContextSize, 0.0F);
  pending_.clear();
  speech_ = false;
  hasSpeech_ = false;
  startCounter_ = 0;
  silenceCounter_ = 0;
  frameCounter_ = 0;
  buffer_.clear();
  preRoll_.clear();
}
