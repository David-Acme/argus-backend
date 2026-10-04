#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <shared/services/vad/vad-service.hxx>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

struct TurnTranscriptInput
{
  IVoiceStt& stt;
  VoiceSttStreamInput stream;
  int flushSilenceFrames{4};
};

class TurnTranscript
{
public:
  explicit TurnTranscript(TurnTranscriptInput input);

  void follow(const VadService& vad);
  [[nodiscard]] std::optional<std::string> finish(std::span<const float> turn);
  [[nodiscard]] bool open() const { return stream_ != nullptr; }
  [[nodiscard]] bool flushed() const { return flushed_; }

  [[nodiscard]] static int flushFramesFor(int minSilenceFrames);

private:
  void fail(const char* step);

  IVoiceStt& stt_;
  VoiceSttStreamInput input_;
  int flushSilenceFrames_;
  std::unique_ptr<IVoiceSttStream> stream_;
  std::size_t pushed_{0};
  bool flushed_{false};
  bool tried_{false};
};
