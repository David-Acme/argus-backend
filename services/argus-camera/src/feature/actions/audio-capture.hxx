#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AudioCaptureInput
{
  std::string url;
  int seconds{0};
  // When true, the capture stops early on a detected end of speech.
  bool endpoint{false};
};

enum class AudioCaptureStatus : uint8_t
{
  Success = 0,
  Timeout,
  SpawnFailed,
  ReadFailed,
  InvalidAudio
};

struct AudioCaptureResult
{
  bool ok{false};
  AudioCaptureStatus status{AudioCaptureStatus::InvalidAudio};
  bool speechDetected{false};
  bool endpointed{false};
  std::vector<int16_t> samples;
  std::string error;
};

namespace audio_capture
{

using CaptureFunction =
    std::function<AudioCaptureResult(const AudioCaptureInput&)>;

// Test-only override; empty in production and always restored by tests.
void setCaptureFunctionForTest(CaptureFunction function);

// Bounded 16 kHz mono s16 capture through ffmpeg; the process is killed on
// deadline.
AudioCaptureResult capture(const AudioCaptureInput& input);

} // namespace audio_capture
