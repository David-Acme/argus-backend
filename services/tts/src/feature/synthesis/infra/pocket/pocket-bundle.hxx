#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class PocketDtype : std::uint8_t
{
  Float32,
  Int64
};

struct PocketStateSpec
{
  std::string module;
  std::string key;
  std::string inputName;
  std::string outputName;
  std::vector<std::int64_t> shape;
  PocketDtype dtype{PocketDtype::Float32};
  bool onesFill{false};
};

struct PocketBundle
{
  std::filesystem::path directory;
  std::string tokenizerFile;
  int sampleRate{24000};
  int samplesPerFrame{1920};
  int latentDim{32};
  int conditioningDim{1024};
  int mimiStepsPerLatent{16};
  int flowCapacity{0};
  int mimiCapacity{0};
  int minFramesBeforeEos{6};
  float temperature{0.3F};
  float eosThreshold{-4.0F};
  double tokensPerSecond{3.0};
  double genSecondsPadding{2.0};
  bool voiceCloning{false};
  bool appendTerminalPunctuation{true};
  bool capitalizeFirstLetter{true};
  bool removeSemicolons{false};
  bool padWithSpaces{false};
  std::optional<int> recommendedFramesAfterEos;
  std::vector<std::pair<std::string, std::string>> replaceCharacters;
  std::vector<PocketStateSpec> flowStates;
  std::vector<PocketStateSpec> mimiStates;
};

[[nodiscard]] PocketBundle loadPocketBundle(const std::filesystem::path& directory);

[[nodiscard]] std::size_t stateElements(const PocketStateSpec& spec);
