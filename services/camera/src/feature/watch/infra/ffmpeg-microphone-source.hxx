#pragma once

#include "microphone-source.hxx"

#include <optional>
#include <string>
#include <sys/types.h>

class FfmpegMicrophoneSource final : public MicrophoneSource
{
public:
  explicit FfmpegMicrophoneSource(std::string url);
  ~FfmpegMicrophoneSource() override;

  FfmpegMicrophoneSource(const FfmpegMicrophoneSource&) = delete;
  FfmpegMicrophoneSource& operator=(const FfmpegMicrophoneSource&) = delete;

  [[nodiscard]] bool open() override;
  [[nodiscard]] MicrophoneRead read(std::chrono::milliseconds timeout) override;
  void close() override;

private:
  std::string url_;
  pid_t child_{-1};
  int fd_{-1};
  std::optional<uint8_t> leftover_;
};

[[nodiscard]] MicrophoneSourceFactory ffmpegMicrophoneSources();
