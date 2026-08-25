#include "noise-suppression-service.hxx"

#include <algorithm>
#include <cmath>
#if ARGUS_HAS_RNNOISE
#include <rnnoise.h>
#endif

namespace
{

constexpr int kFrameSize = 480; // 10 ms at 48 kHz

// AGC: soft speech is boosted toward this RMS so RNNoise (and the STT)
// see a healthy level. 0.12 ~ -18 dBFS.
constexpr float kAgcTargetRms = 0.12F;
// Never amplify more than ~24 dB: it would lift the noise floor into a
// false speech signal. Loud frames compress down to 0.25x so the AGC
// never clips.
constexpr float kAgcMaxGain = 16.0F;
constexpr float kAgcMinGain = 0.25F;
// Fast attack, slow release: gain follows speech up quickly and does not
// pump up and down between words.
constexpr float kAgcAttack = 0.25F;
constexpr float kAgcRelease = 0.008F;

// Voice probability -> original/denoised blend. Below 0.35 the frame is
// treated as noise (denoised), above 0.8 as voice (original, untouched).
constexpr float kMixLow = 0.35F;
constexpr float kMixHigh = 0.8F;

// Compact pending48_ once more than this many frames are consumed.
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

} // namespace

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

void NoiseSuppressor::applyAgc(const std::vector<float>& in,
                               std::vector<float>& out)
{
  out.resize(in.size());
  double sumSq = 0.0;
  for (const float v : in)
    sumSq += static_cast<double>(v) * v;
  const float rms =
      in.empty() ? 0.0F : static_cast<float>(std::sqrt(sumSq / in.size()));

  // Track the envelope: fast attack on speech onsets, slow decay after.
  const float alpha = rms > agcRms_ ? kAgcAttack : kAgcRelease;
  agcRms_ += alpha * (rms - agcRms_);

  // Compress toward the target: quiet speech is boosted, loud frames are
  // tamed, so RNNoise always sees a healthy level without clipping.
  const float desired = kAgcTargetRms / (agcRms_ + 1e-4F);
  const float targetGain =
      std::max(kAgcMinGain, std::min(kAgcMaxGain, desired));
  agcGain_ += 0.05F * (targetGain - agcGain_);

  for (size_t i = 0; i < in.size(); ++i)
    out[i] = clampS16(in[i] * agcGain_);
}

void NoiseSuppressor::process(const std::vector<float>& in,
                              std::vector<float>& out)
{
#if !ARGUS_HAS_RNNOISE
  out = in;
  return;
#else
  out.clear();
  if (!state_ || in.empty()) {
    out = in;
    return;
  }

  // AGC lifts quiet speech before RNNoise sees it.
  applyAgc(in, workBoosted_);

  // 16 kHz float -> int16 -> upsample to 48 kHz.
  workI16_.resize(workBoosted_.size());
  for (size_t i = 0; i < workBoosted_.size(); ++i)
    workI16_[i] = static_cast<int16_t>(clampS16(workBoosted_[i]) * 32767.0F);
  upsampler_.process(workI16_.data(), workI16_.size(), workUp48_);

  // Buffer the 48 kHz audio in RNNoise scale: it expects float samples in
  // the 16-bit range (+-32767), NOT [-1, 1]. Feeding [-1, 1] makes it see
  // silence, keep all gains open and pass the audio through untouched
  // (xiph/rnnoise#184).
  pending48_.reserve(pending48_.size() + workUp48_.size());
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

    // Blend: voice frames pass through untouched, noise-only frames come
    // out suppressed. The voice probability is per frame, so the blend
    // tracks onsets/offsets smoothly (no gate clicks).
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

  // Downsample the blended 48 kHz audio back to 16 kHz.
  if (workDenoised_.empty())
    return;
  workD48_.resize(workDenoised_.size());
  for (size_t i = 0; i < workDenoised_.size(); ++i)
    workD48_[i] = static_cast<int16_t>(
        std::max(-32767.0F, std::min(32767.0F, workDenoised_[i])));
  downsampler_.process(workD48_.data(), workD48_.size(), workDown16_);

  out.reserve(workDown16_.size());
  for (const int16_t s : workDown16_)
    out.push_back(static_cast<float>(s) / 32768.0F);
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
