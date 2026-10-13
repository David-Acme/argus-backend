#include "bundle-golden.hxx"

#include "laya-sequence.hxx"
#include "sp-tokenizer.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace turn
{

namespace
{
constexpr std::string_view kLayaKind = "laya";
constexpr std::string_view kGlinerKind = "gliner";

std::optional<nlohmann::ordered_json> readJson(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  nlohmann::ordered_json root = nlohmann::ordered_json::parse(in, nullptr, false);
  if (root.is_discarded())
    return std::nullopt;
  return root;
}

std::optional<nlohmann::ordered_json> goldenOf(const BundleLoader& bundle, std::string_view kind, GoldenReport& report)
{
  if (!bundle.golden().present) {
    report.error = "the bundle carries no golden vectors";
    return std::nullopt;
  }
  std::optional<nlohmann::ordered_json> root = readJson(bundle.golden().file);
  if (!root || !root->is_object() || root->value("kind", "") != kind || !root->contains("probes") || !(*root)["probes"].is_array() ||
      (*root)["probes"].empty()) {
    report.error = bundle.golden().file.filename().string() + " is not a " + std::string(kind) + " golden file with probes";
    return std::nullopt;
  }
  return root;
}

template <typename Value>
std::vector<Value> listOf(const nlohmann::ordered_json& node)
{
  return node.is_array() ? node.get<std::vector<Value>>() : std::vector<Value>{};
}

bool sameSequence(const LayaSequence& built, const nlohmann::ordered_json& expected)
{
  return built.ids == listOf<std::int32_t>(expected.value("ids", nlohmann::ordered_json::array())) &&
         built.markers == listOf<std::size_t>(expected.value("markers", nlohmann::ordered_json::array()));
}

std::string bestLabel(const std::vector<std::pair<std::string, double>>& probabilities)
{
  const auto best = std::ranges::max_element(probabilities, {}, &std::pair<std::string, double>::second);
  return best == probabilities.end() ? std::string() : best->first;
}
}

GoldenReport checkLayaGolden(const LayaGoldenInput& input)
{
  GoldenReport report;
  const std::optional<nlohmann::ordered_json> golden = goldenOf(input.bundle, kLayaKind, report);
  if (!golden)
    return report;
  SpTokenizer tokenizer;
  const std::optional<nlohmann::ordered_json> questions = readJson(input.bundle.questionsJson());
  if (!tokenizer.load(input.bundle.tokenizerJson()) || !questions || !questions->is_object()) {
    report.error = "the bundle's tokenizer or questions.json cannot be read";
    return report;
  }
  if (input.model.status() != EngineStatus::Ready) {
    report.error = "the bundle's model does not open";
    return report;
  }
  const BundleGolden& tolerance = input.bundle.golden();
  for (const nlohmann::ordered_json& probe : (*golden)["probes"]) {
    const std::string text = probe.value("text", "");
    const std::string lang = probe.value("lang", "es");
    ++report.probes;
    const nlohmann::ordered_json sequences = probe.value("sequences", nlohmann::ordered_json::object());
    for (const auto& [qid, expected] : sequences.items()) {
      const std::optional<LayaQuestion> question = questions->contains(qid) ? layaQuestionFromJson(questions->at(qid)) : std::nullopt;
      if (!question || !sameSequence(layaBuildSequence({.tokenizer = tokenizer,
                                                        .state = text,
                                                        .maxLen = input.bundle.maxLen(),
                                                        .headMaxLen = input.bundle.decode().headMaxLen},
                                                       *question),
                                     expected))
        ++report.sequenceMismatches;
    }
    const std::optional<LayaReading> reading = input.model.read(text, lang);
    if (!reading) {
      ++report.decisionMismatches;
      continue;
    }
    const nlohmann::ordered_json tool = probe.value("tool", nlohmann::ordered_json::object());
    for (const auto& [label, value] : tool.items()) {
      const auto found = std::ranges::find(reading->probabilities, label, &std::pair<std::string, double>::first);
      const double gap = found == reading->probabilities.end() ? 1.0 : std::abs(found->second - value.get<double>());
      report.worstProbability = std::max(report.worstProbability, gap);
    }
    if (probe.contains("choice") && bestLabel(reading->probabilities) != probe.value("choice", ""))
      ++report.decisionMismatches;
    if (probe.contains("now"))
      report.worstNow = std::max(report.worstNow, std::abs(reading->now - probe.value("now", 0.0)));
  }
  report.passed = report.sequenceMismatches == 0 && report.decisionMismatches == 0 &&
                  report.worstProbability <= tolerance.probability && report.worstNow <= tolerance.now;
  return report;
}

GoldenReport checkGlinerGolden(const GlinerGoldenInput& input)
{
  GoldenReport report;
  const std::optional<nlohmann::ordered_json> golden = goldenOf(input.bundle, kGlinerKind, report);
  if (!golden)
    return report;
  SpTokenizer tokenizer;
  if (!tokenizer.load(input.bundle.tokenizerJson())) {
    report.error = "the bundle's tokenizer cannot be read";
    return report;
  }
  if (input.model.status() != EngineStatus::Ready) {
    report.error = "the bundle's model does not open";
    return report;
  }
  for (const nlohmann::ordered_json& probe : (*golden)["probes"]) {
    ++report.probes;
    const std::string text = probe.value("text", "");
    const std::string type = probe.value("type", "");
    const std::vector<std::string> fields = listOf<std::string>(probe.value("fields", nlohmann::ordered_json::array()));
    const std::string document = glinerDocument(text);
    const GlinerPrompt prompt = glinerPrompt({.tokenizer = tokenizer, .document = document, .type = type, .fields = fields});
    if (prompt.ids != listOf<std::int64_t>(probe.value("inputIds", nlohmann::ordered_json::array())) ||
        prompt.wordIndices != listOf<std::int64_t>(probe.value("wordIndices", nlohmann::ordered_json::array())) ||
        prompt.wordMask != listOf<std::int64_t>(probe.value("wordMask", nlohmann::ordered_json::array())) ||
        prompt.queryIndices != listOf<std::int64_t>(probe.value("queryIndices", nlohmann::ordered_json::array())) ||
        prompt.queryMask != listOf<std::int64_t>(probe.value("queryMask", nlohmann::ordered_json::array())))
      ++report.sequenceMismatches;
    const std::vector<GlinerSpan> spans = input.model.spans({.text = text, .lang = probe.value("lang", "es"), .type = type});
    const nlohmann::ordered_json reference = probe.value("reference", nlohmann::ordered_json::object());
    for (const std::string& field : fields) {
      std::vector<const GlinerSpan*> got;
      for (const GlinerSpan& span : spans)
        if (span.field == field)
          got.push_back(&span);
      const nlohmann::ordered_json expected = reference.value(field, nlohmann::ordered_json::array());
      if (got.size() != expected.size()) {
        ++report.spanMismatches;
        continue;
      }
      for (std::size_t at = 0; at < got.size(); ++at) {
        const nlohmann::ordered_json& held = expected[at];
        if (got[at]->text != held.value("text", "") || got[at]->begin != held.value("start", std::size_t{0}) ||
            got[at]->end != held.value("end", std::size_t{0}))
          ++report.spanMismatches;
        report.worstScore = std::max(report.worstScore, std::abs(got[at]->score - held.value("confidence", 0.0)));
      }
    }
  }
  report.passed = report.sequenceMismatches == 0 && report.spanMismatches == 0 && report.worstScore <= input.bundle.golden().score;
  return report;
}

std::string describeGolden(const GoldenReport& report)
{
  if (!report.error.empty())
    return report.error;
  std::ostringstream out;
  out << report.probes << " probes, " << report.sequenceMismatches << " input mismatches, " << report.decisionMismatches
      << " decision mismatches, " << report.spanMismatches << " span mismatches, worst probability gap " << report.worstProbability
      << ", worst now gap " << report.worstNow << ", worst score gap " << report.worstScore << (report.passed ? ": passed" : ": FAILED");
  return out.str();
}

}
