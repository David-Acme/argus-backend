#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class VoiceEncoding : uint8_t
{
  Wav = 0,
  Pcm16
};

struct EncodedVoice
{
  std::string bytes;
  VoiceEncoding encoding{VoiceEncoding::Wav};
  int sampleRate{0};
};

struct PcmClip
{
  std::vector<int16_t> samples;
  int sampleRate{0};
};

namespace voice_audio
{
inline constexpr int kModelRate = 16000;
inline constexpr int kMinRate = 8000;
inline constexpr int kMaxRate = 48000;
inline constexpr int kMaxClipSeconds = 30;

[[nodiscard]] std::optional<PcmClip> decodeWav(std::string_view bytes);

[[nodiscard]] std::optional<PcmClip> decodePcm16(std::string_view bytes,
                                                 int sampleRate);

[[nodiscard]] std::optional<PcmClip> decode(const EncodedVoice& voice);

[[nodiscard]] std::vector<float> toModelRate(const PcmClip& clip);
}
