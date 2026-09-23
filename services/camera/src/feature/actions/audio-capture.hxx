#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AudioCaptureInput
{
  std::string url;
  int seconds{0};
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

void setCaptureFunctionForTest(CaptureFunction function);

AudioCaptureResult capture(const AudioCaptureInput& input);

}
