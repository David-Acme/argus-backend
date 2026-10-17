#include "laya-decider.hxx"

#include "sp-tokenizer.hxx"

#include <nlohmann/json.hpp>
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
constexpr const char* kToolQuestion = "tool";
constexpr const char* kNowQuestion = "now";

struct LayaRank
{
  std::string tool;
  std::string label;
  double score{0.0};
};

std::optional<nlohmann::ordered_json> readOrderedJson(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  nlohmann::ordered_json root = nlohmann::ordered_json::parse(in, nullptr, false);
  if (root.is_discarded())
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
        maxLen_(input.bundle.maxLen()), precision_(input.bundle.precision()), decode_(input.decode), options_(input.options)
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
    const LayaQuestion& tool = *tool_;
    const LayaQuestion& now = *now_;
    const std::array<const LayaQuestion*, 2> questions{&tool, &now};
    const std::optional<std::vector<std::vector<float>>> logits = logitsOf(questions, text);
    if (!logits || (*logits)[0].size() < tool.labels.size() || (*logits)[1].size() < 2)
      return std::nullopt;
    const std::vector<double> toolProbs =
        softmax(std::span((*logits)[0]).first(tool.labels.size()), decode_.temperature[0]);
    std::vector<std::pair<std::string, double>> probabilities;
    probabilities.reserve(tool.labels.size());
    for (std::size_t index = 0; index < tool.labels.size(); ++index)
      probabilities.emplace_back(tool.labels[index], toolProbs[index]);
    const std::vector<double> nowProbs = softmax(std::span((*logits)[1]).first(2), decode_.temperature[2]);
    return LayaReading{.probabilities = std::move(probabilities), .now = nowProbs[1]};
  }

  [[nodiscard]] std::int64_t warmRssBytes() const override { return session_.warmRssDeltaBytes(); }

private:
  void open()
  {
    opened_ = true;
    if (!tokenizer_.load(tokenizerJson_)) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << tokenizerJson_.string()
               << " is not a tokenizer; the turn continues on rules and explicit triggers";
      return;
    }
    const std::optional<nlohmann::ordered_json> questions = readOrderedJson(questionsJson_);
    if (!questions || !questions->is_object()) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << questionsJson_.string()
               << " cannot be read; the turn continues on rules and explicit triggers";
      return;
    }
    if (questions->contains(kToolQuestion))
      tool_ = layaQuestionFromJson(questions->at(kToolQuestion));
    if (questions->contains(kNowQuestion))
      now_ = layaQuestionFromJson(questions->at(kNowQuestion));
    if (!tool_ || tool_->kind != QuestionKind::Choice || tool_->labels.empty() || !now_ || now_->kind != QuestionKind::Noul) {
      LOG_WARN << "argus-llm: the Laya decider is off: " << questionsJson_.string()
               << " has no choice 'tool' and noul 'now' question; the turn continues on rules and explicit triggers";
      return;
    }
    if (!session_.open(modelPath_, options_)) {
      LOG_WARN << "argus-llm: the Laya decider could not open " << modelPath_.filename().string() << ": " << session_.error()
               << "; the turn continues on rules and explicit triggers";
      return;
    }
    LOG_INFO << "argus-llm: the Laya decider is ready on " << modelPath_.filename().string() << " with " << tool_->labels.size()
             << " options, precision=" << std::string(bundlePrecisionToString(precision_)) << " max_len=" << maxLen_
             << " head_max_len=" << decode_.headMaxLen;
    ready_ = true;
  }

  [[nodiscard]] std::optional<std::vector<std::vector<float>>> logitsOf(std::span<const LayaQuestion* const> questions, std::string_view text)
  {
    std::vector<LayaSequence> sequences;
    sequences.reserve(questions.size());
    std::size_t length = 0;
    std::size_t width = 0;
    for (const LayaQuestion* question : questions) {
      LayaSequence sequence =
          layaBuildSequence({.tokenizer = tokenizer_, .state = text, .maxLen = maxLen_, .headMaxLen = decode_.headMaxLen}, *question);
      if (sequence.ids.empty() || sequence.markers.empty())
        return std::nullopt;
      length = std::max(length, sequence.ids.size());
      width = std::max(width, sequence.markers.size());
      sequences.push_back(std::move(sequence));
    }
    const std::size_t rows = sequences.size();
    std::vector<std::int64_t> ids(rows * length, tokenizer_.padId());
    std::vector<std::int64_t> attention(rows * length, 0);
    std::vector<std::int64_t> positions(rows * width, 0);
    std::vector<std::int64_t> markerMask(rows * width, 0);
    std::vector<std::int64_t> qtype;
    qtype.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
      const LayaSequence& sequence = sequences[row];
      std::ranges::copy(sequence.ids, ids.begin() + static_cast<std::ptrdiff_t>(row * length));
      std::fill_n(attention.begin() + static_cast<std::ptrdiff_t>(row * length), sequence.ids.size(), 1);
      std::ranges::copy(sequence.markers, positions.begin() + static_cast<std::ptrdiff_t>(row * width));
      std::fill_n(markerMask.begin() + static_cast<std::ptrdiff_t>(row * width), sequence.markers.size(), 1);
      qtype.push_back(static_cast<std::int64_t>(questions[row]->kind));
    }
    const auto batch = static_cast<std::int64_t>(rows);
    const auto tokens = static_cast<std::int64_t>(length);
    const auto slots = static_cast<std::int64_t>(width);
    const OnnxCall call{
        .inputs = {{.name = "input_ids", .shape = {batch, tokens}, .values = std::move(ids)},
                   {.name = "attention_mask", .shape = {batch, tokens}, .values = std::move(attention)},
                   {.name = "marker_pos", .shape = {batch, slots}, .values = std::move(positions)},
                   {.name = "marker_mask", .shape = {batch, slots}, .values = std::move(markerMask), .boolean = true},
                   {.name = "qtype", .shape = {batch}, .values = std::move(qtype)}},
        .outputs = {"logits"}};
    const std::optional<std::vector<OnnxTensor>> out = session_.run(call);
    if (!out || out->empty() || out->front().shape.size() != 2 || out->front().shape[0] != batch)
      return std::nullopt;
    const auto columns = static_cast<std::size_t>(out->front().shape[1]);
    const std::vector<float>& values = out->front().values;
    std::vector<std::vector<float>> logits;
    logits.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
      const auto from = values.begin() + static_cast<std::ptrdiff_t>(row * columns);
      logits.emplace_back(from, from + static_cast<std::ptrdiff_t>(std::min(columns, sequences[row].markers.size())));
    }
    return logits;
  }

  std::filesystem::path modelPath_;
  std::filesystem::path tokenizerJson_;
  std::filesystem::path questionsJson_;
  int maxLen_{512};
  BundlePrecision precision_{BundlePrecision::Fp32};
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
                   .confident = top >= policy_.thresholdsFor(ranked.front().tool).ask,
                   .runnerUp = std::move(runnerUp),
                   .now = now_.apply(reading->now)};
}

std::unique_ptr<LayaModel> openLayaModel(LayaModelInput input)
{
  return std::make_unique<LayaBundleModel>(input);
}

}
