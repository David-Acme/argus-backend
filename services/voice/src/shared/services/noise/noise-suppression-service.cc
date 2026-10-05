#include "noise-suppression-service.hxx"

#include <algorithm>
#include <cmath>
#if ARGUS_HAS_RNNOISE
#include <rnnoise.h>
#endif

namespace
{

constexpr int kFrameSize = 480;

constexpr float kAgcTargetRms = 0.12F;
constexpr float kAgcMaxGain = 16.0F;
constexpr float kAgcMinGain = 0.25F;
constexpr float kAgcAttack = 0.25F;
constexpr float kAgcRelease = 0.008F;

constexpr float kMixLow = 0.35F;
constexpr float kMixHigh = 0.8F;

constexpr size_t kPendingCompactFrames = 32;

float clampS16(float v)
{
  return std::max(-1.0F, std::min(1.0F, v));
}

#if ARGUS_HAS_RNNOISE
float clamp01(float v)
{
  return std::max(0.0F, std::min(1.0F, v));
}
#endif

}

NoiseSuppressor::NoiseSuppressor()
{
#if ARGUS_HAS_RNNOISE
  const int frameSize = rnnoise_get_frame_size();
  state_ = (frameSize > 0) ? rnnoise_create(nullptr) : nullptr;
#endif
  pending48_.reserve(kFrameSize * 8);
  workFrame_.resize(kFrameSize);
}

NoiseSuppressor::~NoiseSuppressor()
{
#if ARGUS_HAS_RNNOISE
  if (state_) {
    rnnoise_destroy(state_);
    state_ = nullptr;
  }
#endif
}

void NoiseSuppressor::applyAgc(std::span<const float> in,
                               std::vector<float>& out)
{
  out.resize(in.size());
  double sumSq = 0.0;
  for (const float v : in)
    sumSq += static_cast<double>(v) * v;
  const float rms =
      in.empty() ? 0.0F : static_cast<float>(std::sqrt(sumSq / in.size()));

  const float alpha = rms > agcRms_ ? kAgcAttack : kAgcRelease;
  agcRms_ += alpha * (rms - agcRms_);

  const float desired = kAgcTargetRms / (agcRms_ + 1e-4F);
  const float targetGain =
      std::max(kAgcMinGain, std::min(kAgcMaxGain, desired));
  agcGain_ += 0.05F * (targetGain - agcGain_);

  for (size_t i = 0; i < in.size(); ++i)
    out[i] = clampS16(in[i] * agcGain_);
}

void NoiseSuppressor::process(std::span<const float> in,
                              std::vector<float>& out)
{
#if !ARGUS_HAS_RNNOISE
  out.assign(in.begin(), in.end());
  return;
#else
  out.clear();
  if (!state_ || in.empty()) {
    out.assign(in.begin(), in.end());
    return;
  }

  applyAgc(in, workBoosted_);

  workI16_.resize(workBoosted_.size());
  for (size_t i = 0; i < workBoosted_.size(); ++i)
    workI16_[i] = static_cast<int16_t>(clampS16(workBoosted_[i]) * 32767.0F);
  upsampler_.processInto(workI16_, workUp48_);

  for (const int16_t s : workUp48_)
    pending48_.push_back(static_cast<float>(s));

  const size_t available = pending48_.size() - pending48Offset_;
  const size_t frames = available / kFrameSize;
  workDenoised_.clear();
  workDenoised_.reserve(frames * kFrameSize);
  float probSum = 0.0F;
  int probCount = 0;
  size_t offset = pending48Offset_;
  for (size_t f = 0; f < frames; ++f, offset += kFrameSize) {
    const float* orig = pending48_.data() + offset;
    const float prob =
        rnnoise_process_frame(state_, workFrame_.data(), orig);
    probSum += prob;
    ++probCount;

    const float mix = clamp01((prob - kMixLow) / (kMixHigh - kMixLow));
    if (mix > 0.0F && mix < 1.0F) {
      for (int i = 0; i < kFrameSize; ++i)
        workFrame_[static_cast<size_t>(i)] =
            mix * orig[i] + (1.0F - mix) * workFrame_[static_cast<size_t>(i)];
    }
    else if (mix >= 1.0F) {
      std::copy(orig, orig + kFrameSize, workFrame_.data());
    }
    workDenoised_.insert(workDenoised_.end(), workFrame_.begin(),
                         workFrame_.end());
  }
  pending48Offset_ = offset;
  if (pending48Offset_ >= kFrameSize * kPendingCompactFrames) {
    pending48_.erase(pending48_.begin(),
                     pending48_.begin() + static_cast<long>(pending48Offset_));
    pending48Offset_ = 0;
  }
  lastVoiceProb_ = probCount > 0 ? probSum / static_cast<float>(probCount)
                                  : 0.0F;

  if (workDenoised_.empty())
    return;
  workD48_.resize(workDenoised_.size());
  for (size_t i = 0; i < workDenoised_.size(); ++i)
    workD48_[i] = static_cast<int16_t>(
        std::max(-32767.0F, std::min(32767.0F, workDenoised_[i])));
  downsampler_.processInto(workD48_, workDown16_);

  out.resize(workDown16_.size());
  std::ranges::transform(workDown16_, out.begin(),
                         [](int16_t s) { return static_cast<float>(s) / 32768.0F; });
#endif
}

void NoiseSuppressor::reset()
{
#if ARGUS_HAS_RNNOISE
  if (state_) {
    rnnoise_destroy(state_);
    state_ = rnnoise_create(nullptr);
  }
#endif
  upsampler_.reset();
  downsampler_.reset();
  pending48_.clear();
  pending48Offset_ = 0;
  agcRms_ = 0.0F;
  agcGain_ = 1.0F;
  lastVoiceProb_ = 0.0F;
}

void NoiseSuppressor::resync()
{
  upsampler_.reset();
  downsampler_.reset();
  pending48_.clear();
  pending48Offset_ = 0;
}
