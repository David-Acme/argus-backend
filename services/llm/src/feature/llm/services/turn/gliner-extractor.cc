#include "gliner-extractor.hxx"

#include <trantor/utils/Logger.h>

#include <filesystem>
#include <utility>

namespace turn
{

namespace
{
class GlinerBundleModel final : public GlinerModel
{
public:
  GlinerBundleModel(const BundleLoader& bundle, OnnxOptions options)
      : modelPath_(bundle.modelPath()), options_(std::move(options))
  {
  }

  [[nodiscard]] EngineStatus status() override
  {
    if (!opened_)
      open();
    return ready_ ? EngineStatus::Ready : EngineStatus::Unavailable;
  }

  [[nodiscard]] std::vector<GlinerSpan> spans(std::string_view, std::string_view) override { return {}; }

  [[nodiscard]] std::int64_t warmRssBytes() const override { return session_.warmRssDeltaBytes(); }

private:
  void open()
  {
    opened_ = true;
    if (!session_.open(modelPath_, options_)) {
      LOG_WARN << "argus-llm: the GLiNER extractor could not open " << modelPath_.filename().string() << ": " << session_.error()
               << "; the turn continues on NuExtract";
      return;
    }
    LOG_WARN << "argus-llm: the GLiNER extractor opened " << modelPath_.filename().string()
             << " but the bundle exports the encoder alone, with no span head to decode; "
                "the turn continues on NuExtract";
  }

  std::filesystem::path modelPath_;
  OnnxOptions options_;
  OnnxSession session_;
  bool opened_{false};
  bool ready_{false};
};
}

GlinerExtractor::GlinerExtractor(GlinerExtractorInput input)
    : model_(std::move(input.model)), fallback_(input.fallback), thresholds_(input.thresholds)
{
}

std::optional<std::string> GlinerExtractor::extract(const slots::TextRequest& request) const
{
  if (model_ == nullptr || model_->status() != EngineStatus::Ready)
    return fallback_ != nullptr ? fallback_->extract(request) : std::nullopt;
  const std::vector<GlinerSpan> spans = model_->spans(request.utterance, request.lang);
  const GlinerSpan* best = nullptr;
  for (const GlinerSpan& span : spans) {
    if (span.field != request.field || span.score < thresholds_.threshold || span.text.empty())
      continue;
    if (best == nullptr || span.score > best->score)
      best = &span;
  }
  if (best == nullptr)
    return std::nullopt;
  return best->text;
}

std::unique_ptr<GlinerModel> openGlinerModel(const BundleLoader& bundle, const OnnxOptions& options)
{
  return std::make_unique<GlinerBundleModel>(bundle, options);
}

}
