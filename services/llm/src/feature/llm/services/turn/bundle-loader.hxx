#pragma once

#include "calibration.hxx"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

enum class BundleKind : unsigned char
{
  Decider,
  Extractor
};

struct BundleDecode
{
  int headMaxLen{192};
  std::array<double, 3> temperature{1.0, 1.0, 1.0};
};

struct BundlePolicy
{
  double act{0.0};
  double ask{0.0};
  double margin{0.0};
  double now{0.0};
  bool guardMemory{false};
  bool present{false};
};

struct BundleThresholds
{
  double threshold{0.0};
  int maxSpanWidth{0};
  bool present{false};
};

struct BundleLocation
{
  std::filesystem::path dir;
  std::string pin;
  BundleKind kind{BundleKind::Decider};
};

class BundleLoader
{
public:
  explicit BundleLoader(BundleLocation location);

  [[nodiscard]] bool valid() const { return valid_; }

  [[nodiscard]] const std::string& error() const { return error_; }

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

  [[nodiscard]] std::filesystem::path modelPath() const { return dir_ / "model.onnx"; }

  [[nodiscard]] std::filesystem::path tokenizerJson() const { return dir_ / "tokenizer" / "tokenizer.json"; }

  [[nodiscard]] std::filesystem::path questionsJson() const { return dir_ / "questions.json"; }

  [[nodiscard]] int maxLen() const { return maxLen_; }

  [[nodiscard]] const BundleDecode& decode() const { return decode_; }

  [[nodiscard]] const std::vector<std::string>& labels() const { return labels_; }

  [[nodiscard]] const std::map<std::string, std::vector<std::string>>& typeFields() const { return typeFields_; }

  [[nodiscard]] const BundlePolicy& policy() const { return policy_; }

  [[nodiscard]] const BundleThresholds& thresholds() const { return thresholds_; }

  [[nodiscard]] const CalibrationModel& confidenceCalibration() const { return confidenceCalibration_; }

  [[nodiscard]] const CalibrationModel& nowCalibration() const { return nowCalibration_; }

  [[nodiscard]] const std::string& fitSplit() const { return fitSplit_; }

private:
  void load();

  std::filesystem::path dir_;
  std::string pin_;
  BundleKind kind_{BundleKind::Decider};
  bool valid_{false};
  std::string error_;
  int maxLen_{0};
  BundleDecode decode_;
  std::vector<std::string> labels_;
  std::map<std::string, std::vector<std::string>> typeFields_;
  BundlePolicy policy_;
  BundleThresholds thresholds_;
  CalibrationModel confidenceCalibration_;
  CalibrationModel nowCalibration_;
  std::string fitSplit_;
};

}
