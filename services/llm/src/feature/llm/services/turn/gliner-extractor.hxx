#pragma once

#include "bundle-loader.hxx"
#include "onnx-session.hxx"
#include "slots.hxx"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

struct GlinerSpan
{
  std::string field;
  std::string text;
  double score{0.0};
  std::size_t begin{0};
  std::size_t end{0};
};

class GlinerModel
{
public:
  GlinerModel() = default;
  virtual ~GlinerModel() = default;

  GlinerModel(const GlinerModel&) = delete;
  GlinerModel& operator=(const GlinerModel&) = delete;

  [[nodiscard]] virtual EngineStatus status() = 0;

  [[nodiscard]] virtual std::int64_t warmRssBytes() const { return 0; }

  [[nodiscard]] virtual std::vector<GlinerSpan> spans(std::string_view text, std::string_view lang) = 0;
};

struct GlinerExtractorInput
{
  std::unique_ptr<GlinerModel> model;
  const slots::TextSlots* fallback{nullptr};
  BundleThresholds thresholds;
};

class GlinerExtractor final : public slots::TextSlots
{
public:
  explicit GlinerExtractor(GlinerExtractorInput input);

  [[nodiscard]] std::optional<std::string> extract(const slots::TextRequest& request) const override;

  [[nodiscard]] std::string_view id() const { return "gliner"; }

  [[nodiscard]] std::int64_t warmRssBytes() const { return model_ != nullptr ? model_->warmRssBytes() : 0; }

private:
  mutable std::unique_ptr<GlinerModel> model_;
  const slots::TextSlots* fallback_;
  BundleThresholds thresholds_;
};

[[nodiscard]] std::unique_ptr<GlinerModel> openGlinerModel(const BundleLoader& bundle, const OnnxOptions& options);

}
