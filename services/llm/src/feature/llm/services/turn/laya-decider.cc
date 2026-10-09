#include "laya-decider.hxx"

#include "sp-tokenizer.hxx"

#include <json/reader.h>
#include <json/value.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>

namespace turn
{

namespace
{
constexpr std::string_view kNoneLabel = "none";
constexpr std::string_view kAskLabel = "ask";
constexpr std::string_view kToolQuestion = "tool";
constexpr std::string_view kNowQuestion = "now";

struct LayaRank
{
  std::string tool;
  std::string label;
  double score{0.0};
};

std::optional<Json::Value> readJsonFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, in, &root, &errors))
    return std::nullopt;
  return root;
}

std::vector<double> softmax(std::span<const float> logits, double temperature)
{
  const double scale = temperature > 0.0 ? temperature : 1.0;
  std::vector<double> out(logits.size(), 0.0);
  if (logits.empty())
    return out;
  const double top = static_cast<double>(*std::ranges::max_element(logits));
  double total = 0.0;
  for (std::size_t index = 0; index < logits.size(); ++index) {
    out[index] = std::exp((static_cast<double>(logits[index]) - top) / scale);
    total += out[index];
  }
  if (total <= 0.0)
    return out;
  for (double& value : out)
    value /= total;
  return out;
}

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
  explicit LayaBundleModel(LayaModelInput input)
      : modelPath_(input.bundle.modelPath()), tokenizerJson_(input.bundle.tokenizerJson()), questionsJson_(input.bundle.questionsJson()),
        maxLen_(input.bundle.maxLen()), decode_(input.decode), options_(std::move(input.options))
  {
  }

  [[nodiscard]] EngineStatus status() override
  {
    if (!opened_)
      open();
    return ready_ ? EngineStatus::Ready : EngineStatus::Unavailable;
  }

  [[nodiscard]] std::optional<LayaReading> read(std::string_view text, std::string_view) override
  {
    if (!ready_ || !tool_ || !now_)
      return std::nullopt;
    const std::optional<std::vector<float>> toolLogits = logitsOf(*tool_, text);
    if (!toolLogits || toolLogits->size() < tool_->criteria.size())
      return std::nullopt;
    const std::vector<double> toolProbs =
        softmax(std::span(*toolLogits).first(tool_->criteria.size()), decode_.temperature[0]);
    std::vector<std::pair<std::string, double>> probabilities;
    probabilities.reserve(tool_->criteria.size());
    for (std::size_t index = 0; index < tool_->criteria.size(); ++index)
      probabilities.push_back({tool_->criteria[index].first, toolProbs[index]});
    const std::optional<std::vector<float>> nowLogits = logitsOf(*now_, text);
    if (!nowLogits || nowLogits->size() < 2)
      return std::nullopt;
    const std::vector<double> nowProbs = softmax(std::span(*nowLogits).first(2), decode_.temperature[2]);
    return LayaReading{.probabilities = std::move(probabilities), .now = nowProbs[1]};
  }

  [[nodiscard]] std::int64_t warmRssBytes() const override { return session_.warmRssDeltaBytes(); }

private:
  void open()
  {
    opened_ = true;
    if (!tokenizer_.load(tokenizerJson_)) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << tokenizerJson_.string()
               << " is not a tokenizer; the turn continues on rules and the router";
      return;
    }
    const std::optional<Json::Value> questions = readJsonFile(questionsJson_);
    if (!questions) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << questionsJson_.string()
               << " cannot be read; the turn continues on rules and the router";
      return;
    }
    tool_ = layaQuestionFromJson((*questions)[std::string(kToolQuestion)]);
    now_ = layaQuestionFromJson((*questions)[std::string(kNowQuestion)]);
    if (!tool_ || tool_->kind != QuestionKind::Choice || tool_->criteria.empty() || !now_ || now_->kind != QuestionKind::Noul) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << questionsJson_.string()
               << " has no choice 'tool' and noul 'now' question; the turn continues on rules and the router";
      return;
    }
    if (!session_.open(modelPath_, options_)) {
      LOG_WARN << "argus-llm: the Laya decider could not open " << modelPath_.filename().string() << ": " << session_.error()
               << "; the turn continues on rules and the router";
      return;
    }
    LOG_INFO << "argus-llm: the Laya decider is ready on " << modelPath_.filename().string() << " with " << tool_->criteria.size()
             << " options, max_len=" << maxLen_ << " head_max_len=" << decode_.headMaxLen;
    ready_ = true;
  }

  [[nodiscard]] std::optional<std::vector<float>> logitsOf(const LayaQuestion& question, std::string_view text)
  {
    const LayaSequence sequence =
        layaBuildSequence({.tokenizer = tokenizer_, .state = text, .maxLen = maxLen_, .headMaxLen = decode_.headMaxLen}, question);
    if (sequence.ids.empty() || sequence.markers.empty())
      return std::nullopt;
    const std::int64_t length = static_cast<std::int64_t>(sequence.ids.size());
    const std::int64_t markers = static_cast<std::int64_t>(sequence.markers.size());
    std::vector<std::int64_t> ids(sequence.ids.begin(), sequence.ids.end());
    std::vector<std::int64_t> attention(sequence.ids.size(), 1);
    std::vector<std::int64_t> positions(sequence.markers.begin(), sequence.markers.end());
    std::vector<std::int64_t> markerMask(sequence.markers.size(), 1);
    std::vector<std::int64_t> qtype{static_cast<std::int64_t>(question.kind)};
    const OnnxCall call{
        .inputs = {{.name = "input_ids", .shape = {1, length}, .values = std::move(ids)},
                   {.name = "attention_mask", .shape = {1, length}, .values = std::move(attention)},
                   {.name = "marker_pos", .shape = {1, markers}, .values = std::move(positions)},
                   {.name = "marker_mask", .shape = {1, markers}, .values = std::move(markerMask), .boolean = true},
                   {.name = "qtype", .shape = {1}, .values = std::move(qtype)}},
        .outputs = {"logits"}};
    const std::optional<std::vector<OnnxTensor>> out = session_.run(call);
    if (!out || out->empty())
      return std::nullopt;
    return out->front().values;
  }

  std::filesystem::path modelPath_;
  std::filesystem::path tokenizerJson_;
  std::filesystem::path questionsJson_;
  int maxLen_{512};
  BundleDecode decode_;
  OnnxOptions options_;
  SpTokenizer tokenizer_;
  std::optional<LayaQuestion> tool_;
  std::optional<LayaQuestion> now_;
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

std::optional<std::string> unknownLayaLabel(std::span<const std::string> labels)
{
  for (const std::string& label : labels) {
    if (label == kNoneLabel || label == kAskLabel)
      continue;
    if (layaToolFor(label).empty())
      return label;
  }
  return std::nullopt;
}

LayaDecider::LayaDecider(LayaDeciderInput input)
    : model_(std::move(input.model)), fallback_(input.fallback), policy_(input.policy),
      confidence_(std::move(input.confidence)), now_(std::move(input.now))
{
}

std::optional<LayaReading> LayaDecider::read(std::string_view text, std::string_view lang) const
{
  return model_ != nullptr ? model_->read(text, lang) : std::nullopt;
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

std::optional<BundleDecode> layaDecodeFromAgentConfig(const std::filesystem::path& path)
{
  if (path.empty())
    return std::nullopt;
  const std::optional<Json::Value> root = readJsonFile(path);
  if (!root)
    return std::nullopt;
  BundleDecode decode;
  if (const Json::Value& headMaxLen = (*root)["head_max_len"]; headMaxLen.isInt() && headMaxLen.asInt() > 0)
    decode.headMaxLen = headMaxLen.asInt();
  if (const Json::Value& temperature = (*root)["temperature"]; temperature.isArray() && temperature.size() == decode.temperature.size()) {
    std::array<double, 3> values{};
    bool numeric = true;
    for (Json::ArrayIndex index = 0; index < values.size(); ++index) {
      numeric = numeric && temperature[index].isNumeric();
      values[index] = numeric ? temperature[index].asDouble() : 1.0;
    }
    if (!numeric)
      return std::nullopt;
    decode.temperature = values;
  }
  return decode;
}

std::unique_ptr<LayaModel> openLayaModel(LayaModelInput input)
{
  return std::make_unique<LayaBundleModel>(std::move(input));
}

}
