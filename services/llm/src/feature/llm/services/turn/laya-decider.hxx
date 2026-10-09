#pragma once

#include "bundle-loader.hxx"
#include "calibration.hxx"
#include "decider.hxx"
#include "laya-sequence.hxx"
#include "onnx-session.hxx"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace turn
{

struct LayaReading
{
  std::vector<std::pair<std::string, double>> probabilities;
  double now{0.0};
};

class LayaModel
{
public:
  LayaModel() = default;
  virtual ~LayaModel() = default;

  LayaModel(const LayaModel&) = delete;
  LayaModel& operator=(const LayaModel&) = delete;

  [[nodiscard]] virtual EngineStatus status() = 0;

  [[nodiscard]] virtual std::int64_t warmRssBytes() const { return 0; }

  [[nodiscard]] virtual std::optional<LayaReading> read(std::string_view text, std::string_view lang) = 0;
};

struct LayaDeciderInput
{
  std::unique_ptr<LayaModel> model;
  const Decider* fallback{nullptr};
  BundlePolicy policy;
  CalibrationModel confidence;
  CalibrationModel now;
};

struct LayaModelInput
{
  const BundleLoader& bundle;
  OnnxOptions options;
  BundleDecode decode;
};

class LayaDecider final : public Decider
{
public:
  explicit LayaDecider(LayaDeciderInput input);

  [[nodiscard]] std::string_view id() const override { return kDeciderIds[2]; }

  [[nodiscard]] std::optional<Candidate> decide(const DecideInput& input) const override;

  [[nodiscard]] std::optional<LayaReading> read(std::string_view text, std::string_view lang) const;

  [[nodiscard]] bool model_ready() const { return model_ != nullptr && model_->status() == EngineStatus::Ready; }

  [[nodiscard]] const BundlePolicy& policy() const { return policy_; }

  [[nodiscard]] std::int64_t warmRssBytes() const { return model_ != nullptr ? model_->warmRssBytes() : 0; }

private:
  mutable std::unique_ptr<LayaModel> model_;
  const Decider* fallback_;
  BundlePolicy policy_;
  CalibrationModel confidence_;
  CalibrationModel now_;
};

[[nodiscard]] std::string_view layaToolFor(std::string_view label);

struct LayaLabel
{
  std::string_view label;
  std::string_view tool;
};

[[nodiscard]] std::span<const LayaLabel> layaLabels();

[[nodiscard]] std::optional<std::string> unknownLayaLabel(std::span<const std::string> labels);

[[nodiscard]] std::optional<BundleDecode> layaDecodeFromAgentConfig(const std::filesystem::path& path);

[[nodiscard]] std::unique_ptr<LayaModel> openLayaModel(LayaModelInput input);

}
