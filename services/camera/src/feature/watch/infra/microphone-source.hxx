#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

enum class MicrophoneReadStatus : uint8_t
{
  Samples,
  Idle,
  Ended
};

struct MicrophoneRead
{
  MicrophoneReadStatus status{MicrophoneReadStatus::Ended};
  std::vector<int16_t> samples;
};

class MicrophoneSource
{
public:
  virtual ~MicrophoneSource() = default;

  [[nodiscard]] virtual bool open() = 0;
  [[nodiscard]] virtual MicrophoneRead read(std::chrono::milliseconds timeout) = 0;
  virtual void close() = 0;
};

using MicrophoneSourceFactory = std::function<std::unique_ptr<MicrophoneSource>(int64_t cameraId)>;
