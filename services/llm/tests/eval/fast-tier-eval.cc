#include "eval-case.hxx"
#include "eval-report.hxx"

#include <config/config-service.hxx>
#include <feature/llm/services/intent-gate.hxx>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr int kSkipped = 77;

struct Options
{
  std::string cases;
  std::string gates;
  std::string model;
  std::string report;
  std::vector<std::string> real;
  bool verbose = false;
};

struct Tally
{
  int support = 0;
  int predicted = 0;
  int correct = 0;
};

struct Rates
{
  int negatives = 0;
  int falseActions = 0;
};

struct Outcome
{
  std::string routed = "none";
  bool fromRules = false;
  std::string source = "model";
};

struct SourceTally
{
  int routed = 0;
  int correct = 0;
};

Options parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--cases" && hasValue)
      options.cases = argv[++i];
    else if (arg == "--gates" && hasValue)
      options.gates = argv[++i];
    else if (arg == "--model" && hasValue)
      options.model = argv[++i];
    else if (arg == "--report" && hasValue)
      options.report = argv[++i];
    else if (arg == "--real" && hasValue)
      options.real.emplace_back(argv[++i]);
    else if (arg == "--verbose")
      options.verbose = true;
  }
  return options;
}

bool routedIntent(intent::ToolIntent value)
{
  using intent::ToolIntent;
  return value == ToolIntent::MemorySave || value == ToolIntent::MemoryRecall ||
         value == ToolIntent::ReminderSet || value == ToolIntent::MemoryForget;
}

std::string_view sourceName(intent::DecisionSource source)
{
  switch (source) {
    case intent::DecisionSource::Trigger:
      return "trigger";
    case intent::DecisionSource::Cancellation:
      return "cancellation";
    case intent::DecisionSource::Statement:
      return "statement";
    case intent::DecisionSource::RecallMarker:
      return "recallMarker";
    case intent::DecisionSource::Model:
      break;
  }
  return "model";
}

Outcome decide(const IntentGate& gate, const eval::EvalCase& item)
{
  const auto decision = gate.router().decide(item.utterance(), item.lang);
  if (!decision.confident || !routedIntent(decision.intent))
    return {};
  return {.routed = std::string(intent::toolIntentToString(decision.intent)),
          .fromRules = decision.fromRules,
          .source = std::string(sourceName(decision.source))};
}

bool isPositive(const std::string& route)
{
  return route != "none" && route != "camera";
}

std::vector<eval::EvalCase> uniqueUtterances(const std::vector<eval::EvalCase>& cases)
{
  std::set<std::string> seen;
  std::vector<eval::EvalCase> out;
  for (const auto& item : cases) {
    if (seen.insert(item.utterance()).second)
      out.push_back(item);
  }
  return out;
}

void addRate(Rates& rates, bool falseAction)
{
  ++rates.negatives;
  rates.falseActions += falseAction ? 1 : 0;
}

double ratio(int numerator, int denominator)
{
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / denominator;
}

std::vector<eval::EvalCase> readRealTraffic(const std::vector<std::string>& paths)
{
  std::vector<eval::EvalCase> out;
  for (const auto& path : paths) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      const auto tab = line.find('\t');
      if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
        continue;
      eval::EvalCase item;
      item.id = "real-" + std::to_string(out.size() + 1);
      item.group = "real";
      item.lang = "es";
      item.variant = "real";
      item.route = line.substr(0, tab);
      item.script = {line.substr(tab + 1)};
      out.push_back(std::move(item));
    }
  }
  return out;
}

void merge(eval::Metrics& target, const eval::Metrics& source)
{
  for (const auto& [name, value] : source)
    target["real." + name] = value;
}

eval::Metrics measure(const IntentGate& gate, const std::vector<eval::EvalCase>& cases, bool verbose)
{
  const std::vector<std::string> classes{"memory_save", "memory_recall", "reminder_set", "memory_forget"};
  std::map<std::string, Tally> tallies;
  std::map<std::string, Rates> byGroup;
  std::map<std::string, Rates> byVariant;
  std::map<std::string, SourceTally> bySource;
  std::map<std::string, Tally> byVariantTool;
  Rates overall;
  int fallThrough = 0;
  int ruleDecided = 0;
  int routedTotal = 0;
  for (const auto& item : cases) {
    const Outcome outcome = decide(gate, item);
    if (outcome.routed == "none")
      ++fallThrough;
    else {
      ++routedTotal;
      ruleDecided += outcome.fromRules ? 1 : 0;
    }
    if (isPositive(item.route))
      ++tallies[item.route].support;
    if (isPositive(item.route))
      ++byVariantTool[item.variant].support;
    if (outcome.routed != "none")
      ++byVariantTool[item.variant].predicted;
    if (outcome.routed == item.route && isPositive(item.route))
      ++byVariantTool[item.variant].correct;
    if (outcome.routed != "none") {
      ++tallies[outcome.routed].predicted;
      ++bySource[outcome.source].routed;
      bySource[outcome.source].correct += outcome.routed == item.route ? 1 : 0;
    }
    if (outcome.routed == item.route && isPositive(item.route))
      ++tallies[item.route].correct;
    if (!isPositive(item.route)) {
      const bool falseAction = outcome.routed != "none";
      addRate(overall, falseAction);
      addRate(byGroup[item.group], falseAction);
      addRate(byVariant[item.variant], falseAction);
      if (falseAction && verbose)
        std::printf("FALSE ACTION %s [%s] %s -> %s (%s)\n", item.id.c_str(), item.group.c_str(),
                    item.utterance().c_str(), outcome.routed.c_str(), outcome.source.c_str());
    }
    else if (verbose && outcome.routed != item.route)
      std::printf("MISS %s %s -> %s: %s\n", item.id.c_str(), item.route.c_str(),
                  outcome.routed.c_str(), item.utterance().c_str());
  }

  eval::Metrics metrics;
  metrics["cases"] = static_cast<double>(cases.size());
  metrics["fallThroughShare"] = ratio(fallThrough, static_cast<int>(cases.size()));
  metrics["rulesShareOfRouted"] = ratio(ruleDecided, routedTotal);
  metrics["falseActionRate"] = ratio(overall.falseActions, overall.negatives);
  for (const auto& name : classes) {
    const Tally& tally = tallies[name];
    metrics[name + ".precision"] = ratio(tally.correct, tally.predicted);
    metrics[name + ".precisionLower"] = eval::wilson({.hits = tally.correct, .total = tally.predicted}).lower;
    metrics[name + ".recall"] = ratio(tally.correct, tally.support);
    metrics[name + ".support"] = tally.support;
  }
  for (const auto& [variant, tally] : byVariantTool) {
    metrics["variant." + variant + ".precision"] = ratio(tally.correct, tally.predicted);
    metrics["variant." + variant + ".recall"] = ratio(tally.correct, tally.support);
  }
  for (const auto& [source, tally] : bySource) {
    metrics["source." + source + ".routed"] = tally.routed;
    metrics["source." + source + ".precision"] = ratio(tally.correct, tally.routed);
  }
  for (const auto& [group, rates] : byGroup)
    metrics["group." + group + ".falseActionRate"] = ratio(rates.falseActions, rates.negatives);
  for (const auto& [variant, rates] : byVariant)
    metrics["variant." + variant + ".falseActionRate"] = ratio(rates.falseActions, rates.negatives);
  return metrics;
}

}

int main(int argc, char** argv)
{
  const Options options = parseOptions(argc, argv);
  if (!std::filesystem::exists(options.model)) {
    std::printf("[SKIPPED] no intent model at %s; the fast-tier gate did not run\n", options.model.c_str());
    return kSkipped;
  }
  const auto loaded = eval::loadCases(options.cases);
  if (!loaded.error.empty()) {
    std::printf("[ERROR] %s\n", loaded.error.c_str());
    return 1;
  }
  const auto gates = eval::loadGates(options.gates, "fast");
  if (!gates.error.empty()) {
    std::printf("[ERROR] %s\n", gates.error.c_str());
    return 1;
  }

  ConfigService::setRuntimeString("intent.model_file", options.model);
  const IntentGate gate;
  if (!gate.isLoaded()) {
    std::printf("[ERROR] the intent model at %s did not load\n", options.model.c_str());
    return 1;
  }

  const auto cases = uniqueUtterances(loaded.cases);
  eval::Metrics metrics = measure(gate, cases, options.verbose);
  if (!options.real.empty()) {
    const auto real = uniqueUtterances(readRealTraffic(options.real));
    merge(metrics, measure(gate, real, options.verbose));
  }
  const eval::Verdict verdict = eval::check(gates.gates, metrics);

  std::printf("fast tier: %zu distinct utterances from %zu cases\n", cases.size(), loaded.cases.size());
  eval::printMetrics(metrics);
  if (!options.report.empty() &&
      !eval::writeReport(options.report, {.section = "fast", .metrics = metrics, .verdict = verdict})) {
    std::printf("[ERROR] cannot write %s\n", options.report.c_str());
    return 1;
  }
  if (!verdict.passed()) {
    std::printf("GATE FAILED\n");
    for (const auto& failure : verdict.failures)
      std::printf("  %s\n", failure.c_str());
    return 1;
  }
  std::printf("GATE PASSED\n");
  return 0;
}
