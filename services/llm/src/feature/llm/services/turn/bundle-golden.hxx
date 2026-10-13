#pragma once

#include "bundle-loader.hxx"
#include "gliner-extractor.hxx"
#include "laya-decider.hxx"

#include <cstddef>
#include <string>

namespace turn
{

struct GoldenReport
{
  std::size_t probes{0};
  std::size_t sequenceMismatches{0};
  std::size_t decisionMismatches{0};
  std::size_t spanMismatches{0};
  double worstProbability{0.0};
  double worstNow{0.0};
  double worstScore{0.0};
  bool passed{false};
  std::string error;
};

struct LayaGoldenInput
{
  const BundleLoader& bundle;
  LayaModel& model;
};

struct GlinerGoldenInput
{
  const BundleLoader& bundle;
  GlinerModel& model;
};

[[nodiscard]] GoldenReport checkLayaGolden(const LayaGoldenInput& input);

[[nodiscard]] GoldenReport checkGlinerGolden(const GlinerGoldenInput& input);

[[nodiscard]] std::string describeGolden(const GoldenReport& report);

}
