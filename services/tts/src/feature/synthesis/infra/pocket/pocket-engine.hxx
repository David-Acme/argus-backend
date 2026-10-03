#pragma once

#include "pocket-voice.hxx"

#include <onnxruntime_cxx_api.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

struct PocketEngineConfig
{
  Ort::Env& env;
  std::filesystem::path directory;
  std::string precision;
  int threads{1};
};

struct PocketGeneration
{
  float temperature{0.3F};
  int lsdSteps{1};
  std::uint32_t seed{0};
};

struct PocketStreamInput
{
  std::string_view text;
  const PocketVoice& voice;
  PocketGeneration generation;
  std::function<void(std::span<const float>)> onAudio;
  std::function<bool()> stopRequested;
};

class PocketEngine
{
public:
  static constexpr std::size_t kMaxChunkTokens = 50;

  explicit PocketEngine(const PocketEngineConfig& config);
  ~PocketEngine();

  PocketEngine(const PocketEngine&) = delete;
  PocketEngine& operator=(const PocketEngine&) = delete;
  PocketEngine(PocketEngine&&) = delete;
  PocketEngine& operator=(PocketEngine&&) = delete;

  [[nodiscard]] std::size_t tokenCount(std::string_view text) const;
  [[nodiscard]] int sampleRate() const;
  [[nodiscard]] float defaultTemperature() const;
  [[nodiscard]] bool canClone() const;
  [[nodiscard]] PocketVoice loadVoice(const std::filesystem::path& path) const;
  [[nodiscard]] PocketVoice cloneVoice(std::span<const float> samples);

  void stream(const PocketStreamInput& input);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
