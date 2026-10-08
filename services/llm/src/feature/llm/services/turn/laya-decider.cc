#include "laya-decider.hxx"

#include <feature/llm/services/tools/tool-registry.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <utility>

namespace turn
{

namespace
{
constexpr std::string_view kNoneLabel = "none";
constexpr std::string_view kAskLabel = "ask";

struct LayaRank
{
  std::string tool;
  std::string label;
  double score{0.0};
};

constexpr std::array<LayaLabel, 21> kLabelTools{{
    {.label = "memory_save", .tool = "memory.remember"},
    {.label = "memory_recall", .tool = "memory.recall"},
    {.label = "reminder_set", .tool = "memory.remind"},
    {.label = "memory_forget", .tool = "memory.forget"},
    {.label = "camera", .tool = "app.show_camera"},
    {.label = "app_open", .tool = "app.open"},
    {.label = "app_guard_mode", .tool = "app.set_guard_mode"},
    {.label = "calendar_create", .tool = "calendar.create_event"},
    {.label = "calendar_list", .tool = "calendar.list_events"},
    {.label = "calendar_cancel", .tool = "calendar.cancel_event"},
    {.label = "task_create", .tool = "task.create"},
    {.label = "task_list", .tool = "task.list"},
    {.label = "task_complete", .tool = "task.complete"},
    {.label = "project_create", .tool = "project.create"},
    {.label = "project_list", .tool = "project.list"},
    {.label = "modules_list", .tool = "modules.list"},
    {.label = "modules_explain", .tool = "modules.explain"},
    {.label = "modules_enable", .tool = "modules.enable"},
    {.label = "modules_disable", .tool = "modules.disable"},
    {.label = "modules_purge", .tool = "modules.open_purge_screen"},
    {.label = "reminder_list", .tool = "reminder.list"},
}};

class LayaBundleModel final : public LayaModel
{
public:
  LayaBundleModel(const BundleLoader& bundle, OnnxOptions options)
      : modelPath_(bundle.modelPath()), options_(std::move(options))
  {
  }

  [[nodiscard]] EngineStatus status() override
  {
    if (!opened_)
      open();
    return ready_ ? EngineStatus::Ready : EngineStatus::Unavailable;
  }

  [[nodiscard]] std::optional<LayaReading> read(std::string_view, std::string_view) override { return std::nullopt; }

  [[nodiscard]] std::int64_t warmRssBytes() const override { return session_.warmRssDeltaBytes(); }

private:
  void open()
  {
    opened_ = true;
    if (!session_.open(modelPath_, options_)) {
      LOG_WARN << "argus-llm: the Laya decider could not open " << modelPath_.filename().string() << ": " << session_.error()
               << "; the turn continues on rules and the router";
      return;
    }
    LOG_WARN << "argus-llm: the Laya decider opened " << modelPath_.filename().string()
             << " but the bundle carries no question templates to build its inputs from; "
                "the turn continues on rules and the router";
  }

  std::filesystem::path modelPath_;
  OnnxOptions options_;
  OnnxSession session_;
  bool opened_{false};
  bool ready_{false};
};
}

std::string_view layaToolFor(std::string_view label)
{
  for (const LayaLabel& entry : kLabelTools)
    if (entry.label == label)
      return entry.tool;
  return {};
}

std::span<const LayaLabel> layaLabels()
{
  return kLabelTools;
}

std::optional<std::string> unknownLayaLabel(const std::vector<std::string>& labels, const ToolRegistry& registry)
{
  const bool listed = !registry.names().empty();
  for (const std::string& label : labels) {
    if (label == kNoneLabel || label == kAskLabel)
      continue;
    const std::string_view tool = layaToolFor(label);
    if (tool.empty() || (listed && !registry.find(std::string(tool))))
      return label;
  }
  return std::nullopt;
}

LayaDecider::LayaDecider(LayaDeciderInput input)
    : model_(std::move(input.model)), fallback_(input.fallback), policy_(input.policy),
      confidence_(std::move(input.confidence)), now_(std::move(input.now))
{
}

std::optional<Candidate> LayaDecider::decide(const DecideInput& input) const
{
  if (model_ == nullptr || model_->status() != EngineStatus::Ready)
    return fallback_ != nullptr ? fallback_->decide(input) : std::nullopt;
  const std::optional<LayaReading> reading = model_->read(input.utterance, input.lang);
  if (!reading)
    return std::nullopt;
  std::vector<LayaRank> ranked;
  for (const auto& [label, score] : reading->probabilities) {
    if (label == kNoneLabel || label == kAskLabel)
      continue;
    const std::string_view tool = layaToolFor(label);
    if (tool.empty() || !isOffered(input, tool))
      continue;
    ranked.push_back({.tool = std::string(tool), .label = label, .score = score});
  }
  if (ranked.empty())
    return std::nullopt;
  std::ranges::sort(ranked, [](const LayaRank& left, const LayaRank& right) { return left.score > right.score; });
  const double top = confidence_.apply(ranked.front().score);
  std::optional<Pick> runnerUp;
  if (ranked.size() > 1 && ranked[1].tool != ranked.front().tool)
    runnerUp = Pick{.tool = ranked[1].tool,
                    .arguments = Json::Value(Json::objectValue),
                    .fill = {},
                    .confidence = confidence_.apply(ranked[1].score),
                    .source = "laya, " + ranked[1].label};
  return Candidate{.tool = ranked.front().tool,
                   .arguments = Json::Value(Json::objectValue),
                   .fill = {},
                   .confidence = top,
                   .source = "laya, " + ranked.front().label,
                   .decider = std::string(id()),
                   .exact = false,
                   .confident = top >= policy_.ask,
                   .runnerUp = std::move(runnerUp),
                   .now = now_.apply(reading->now)};
}

std::unique_ptr<LayaModel> openLayaModel(const BundleLoader& bundle, const OnnxOptions& options)
{
  return std::make_unique<LayaBundleModel>(bundle, options);
}

}
