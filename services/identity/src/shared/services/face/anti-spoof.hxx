#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ort
{
struct Env;
struct Session;
}

struct AntiSpoofModelPin
{
  std::string_view file;
  std::string_view sha256;
  float scale{0.0F};
};

inline constexpr std::array<AntiSpoofModelPin, 2> kAntiSpoofModels{{
    {.file = "MiniFASNetV2.onnx",
     .sha256 = "b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907",
     .scale = 2.7F},
    {.file = "MiniFASNetV1SE.onnx",
     .sha256 = "ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676",
     .scale = 4.0F},
}};

inline constexpr int kAntiSpoofInputSide = 80;
inline constexpr int kAntiSpoofClasses = 3;
inline constexpr int kAntiSpoofRealClass = 1;

struct AntiSpoofCropInput
{
  int imageWidth{0};
  int imageHeight{0};
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
  float scale{0.0F};
};

struct AntiSpoofCrop
{
  int x1{0};
  int y1{0};
  int x2{0};
  int y2{0};

  [[nodiscard]] int width() const { return x2 - x1 + 1; }
  [[nodiscard]] int height() const { return y2 - y1 + 1; }
};

enum class AntiSpoofLoad : std::uint8_t
{
  Loaded,
  Missing,
  ChecksumMismatch,
  Invalid
};

namespace anti_spoof
{
[[nodiscard]] std::optional<AntiSpoofCrop> cropBox(const AntiSpoofCropInput& input);
[[nodiscard]] std::array<float, kAntiSpoofClasses>
softmax(std::span<const float, kAntiSpoofClasses> logits);
[[nodiscard]] bool isLive(float realScore, float threshold);
[[nodiscard]] std::string_view loadToString(AntiSpoofLoad load);
[[nodiscard]] std::optional<std::string> sha256File(const std::string& path);
}

struct AntiSpoofScoreInput
{
  const std::uint8_t* rgbData{nullptr};
  int width{0};
  int height{0};
  float x1{0.0F};
  float y1{0.0F};
  float x2{0.0F};
  float y2{0.0F};
};

class AntiSpoofEngine
{
public:
  AntiSpoofEngine();
  ~AntiSpoofEngine();

  AntiSpoofEngine(const AntiSpoofEngine&) = delete;
  AntiSpoofEngine& operator=(const AntiSpoofEngine&) = delete;

  AntiSpoofLoad load(const std::string& modelDir);
  void unload();
  [[nodiscard]] bool isLoaded() const { return !sessions_.empty(); }

  [[nodiscard]] std::optional<float> realScore(const AntiSpoofScoreInput& input) const;

private:
  struct Model
  {
    std::unique_ptr<Ort::Session> session;
    float scale{0.0F};
    std::string inputName;
    std::string outputName;
  };

  std::unique_ptr<Ort::Env> env_;
  std::vector<Model> sessions_;
};
