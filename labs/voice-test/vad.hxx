#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <vector>

// Voice activity detection based on Silero VAD v5 (ONNX, ~0.1ms per 32ms
// chunk). Detects speech with a continuous probability and drives turn-taking
// with hysteresis + hangover, matching the OpenAI Realtime server_vad
// defaults (threshold 0.5, silence 500ms).
class Vad
{
public:
  struct Config
  {
    int sampleRate{16000};
    // Speech probability threshold to start a turn.
    float threshold{0.5F};
    // Lower probability to end a turn (hysteresis, avoids jitter).
    float negThreshold{0.35F};
    // Min speech frames (32ms each) to accept a turn start.
    int minSpeechFrames{8};
    // Min silence frames (32ms each) to close a turn (~500ms).
    int minSilenceFrames{16};
    // Hard cap on a turn in frames (32ms each).
    int maxTurnFrames{750};
    // Audio (frames) kept before the detected speech start so the leading
    // phoneme is not clipped (~300ms = 10 frames at 32ms).
    int preRollFrames{10};
  };

  Vad();
  ~Vad();

  Vad(const Vad&) = delete;
  Vad& operator=(const Vad&) = delete;

  // Feeds a chunk of samples. Returns true when a full utterance has been
  // captured (speech ended after the hangover silence).
  bool process(const float* samples, int count, std::vector<float>& outTurn);

  // True while the current turn is actively capturing speech.
  bool inSpeech() const;

  void reset();

private:
  void runModel(const float* window, int windowLen, float& prob);

  Config cfg_;
  Ort::Env env_;
  std::unique_ptr<Ort::Session> session_;
  std::mutex mutex_;
  std::vector<float> state_;
  std::vector<float> context_;
  std::vector<float> pending_;
  bool speech_{false};
  bool hasSpeech_{false};
  int startCounter_{0};
  int silenceCounter_{0};
  int frameCounter_{0};
  std::vector<float> buffer_;
  std::vector<float> preRoll_;
};
