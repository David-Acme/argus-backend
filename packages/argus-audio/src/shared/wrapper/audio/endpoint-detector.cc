#include "endpoint-detector.hxx"

#include <algorithm>
#include <cmath>
#include <utility>

EndpointDetector::EndpointDetector(EndpointConfig config)
    : config_(std::move(config))
{
  frameSamples_ = std::max(1, config_.sampleRate * config_.frameMs / 1000);
}

void EndpointDetector::reset()
{
  frameFill_ = 0;
  frameEnergy_ = 0.0;
  frameCrossings_ = 0;
  previousSample_ = 0;
  hasPrevious_ = false;
  noiseFloor_ = 0.0;
  speechDetected_ = false;
  speaking_ = false;
  speechFrames_ = 0;
  silenceFrames_ = 0;
  speechMs_ = 0;
  totalMs_ = 0;
}

void EndpointDetector::processFrame()
{
  const double rms = std::sqrt(frameEnergy_ / frameSamples_);
  const double zcr = static_cast<double>(frameCrossings_) / frameSamples_;
  if (noiseFloor_ <= 0.0)
    noiseFloor_ = config_.absoluteFloorRms;

  const double startLevel =
      std::max({noiseFloor_ * config_.speechFloorRatio,
                static_cast<double>(config_.absoluteFloorRms), 1.0});
  const double stopLevel =
      std::max({noiseFloor_ * config_.silenceFloorRatio,
                static_cast<double>(config_.absoluteFloorRms) * 0.5, 1.0});
  const bool speechLike =
      rms >= startLevel && zcr >= config_.zcrMin && zcr <= config_.zcrMax;

  totalMs_ += config_.frameMs;
  if (!speaking_) {
    if (!speechLike)
      noiseFloor_ = noiseFloor_ * 0.95 + rms * 0.05;
    speechFrames_ = speechLike ? speechFrames_ + 1 : 0;
    if (speechFrames_ >= config_.startFrames) {
      speaking_ = true;
      speechDetected_ = true;
      speechMs_ = config_.frameMs * config_.startFrames;
      silenceFrames_ = 0;
    }
    return;
  }

  if (rms >= stopLevel) {
    speechMs_ += config_.frameMs;
    silenceFrames_ = 0;
  }
  else {
    silenceFrames_ += 1;
  }
}

EndpointStatus EndpointDetector::process(const int16_t* samples,
                                         std::size_t count)
{
  EndpointStatus status;
  if (samples == nullptr)
    return status;

  for (std::size_t index = 0; index < count; ++index) {
    const int16_t sample = samples[index];
    if (hasPrevious_ && ((sample < 0) != (previousSample_ < 0)))
      ++frameCrossings_;
    previousSample_ = sample;
    hasPrevious_ = true;
    const double value = static_cast<double>(sample);
    frameEnergy_ += value * value;
    if (++frameFill_ < frameSamples_)
      continue;
    const bool wasSpeaking = speaking_;
    processFrame();
    if (!wasSpeaking && speaking_)
      status.speechStarted = true;
    frameFill_ = 0;
    frameEnergy_ = 0.0;
    frameCrossings_ = 0;
  }

  if (speaking_) {
    const int silenceMs = silenceFrames_ * config_.frameMs;
    if (silenceMs >= config_.endSilenceMs ||
        totalMs_ >= config_.maxUtteranceMs) {
      if (speechMs_ >= config_.minSpeechMs)
        status.endpointed = true;
      else
        reset();
    }
  }
  return status;
}

EndpointConfig audio_endpoint::cameraListenDefaults()
{
  EndpointConfig config;
  config.sampleRate = 16000;
  config.frameMs = 30;
  config.startFrames = 3;
  config.endSilenceMs = 700;
  config.maxUtteranceMs = 10000;
  config.minSpeechMs = 300;
  return config;
}
