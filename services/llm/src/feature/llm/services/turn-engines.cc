#include "turn-engines.hxx"

#include <feature/llm/services/lfm-adapter.hxx>

#include <runtime/thread-budget.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
constexpr int kLayaThreads = 4;

void installDecider(const TurnEngineInput& input, TurnEngines& engines)
{
  const turn::BundleLoader bundle({.dir = input.decide.dir});
  if (!bundle.valid()) {
    engines.decideError = bundle.error();
    LOG_WARN << "argus-llm: the Laya decider is off: " << engines.decideError
             << "; the turn runs on rules and explicit triggers";
    return;
  }
  if (const std::optional<std::string> stray = turn::unknownLayaLabel(bundle.labels())) {
    engines.decideError = "the Laya bundle " + input.decide.dir.string() + " names the label '" + *stray + "', which no tool serves";
    LOG_WARN << "argus-llm: " << engines.decideError << "; the turn runs on rules and explicit triggers";
    return;
  }
  std::string unnamed;
  for (const turn::LayaLabel& entry : turn::layaLabels()) {
    if (std::ranges::find(bundle.labels(), entry.label) == bundle.labels().end() ||
        turn::classNamesTool(bundle.policy().toolClasses, entry.tool))
      continue;
    unnamed += unnamed.empty() ? "" : ", ";
    unnamed += entry.tool;
  }
  if (!unnamed.empty())
    LOG_WARN << "argus-llm: the Laya bundle's decision.json names no tool class for " << unnamed
             << "; those tools act at the decider's own thresholds";
  for (const auto& [tools, thresholds] : bundle.policy().toolClasses)
    if (thresholds.act < bundle.policy().act || thresholds.ask < bundle.policy().ask) {
      LOG_WARN << "argus-llm: the Laya bundle declares the tool class " << tools.front() << " (and "
               << tools.size() - 1 << " more) at " << thresholds.act << ", below the decider's own "
               << bundle.policy().act << "; a class may only be stricter, so it is floored to it";
      break;
    }
  LfmAdapter& adapter = input.adapter;
  turn::OnnxOptions options = input.decide.options;
  options.threads = layaSessionThreads(options.threads);
  engines.laya = std::make_unique<turn::LayaDecider>(turn::LayaDeciderInput{
      .model = turn::openLayaModel({.bundle = bundle, .options = options, .decode = bundle.decode()}),
      .fallback = nullptr,
      .policy = bundle.policy(),
      .confidence = bundle.confidenceCalibration(),
      .now = bundle.nowCalibration()});
  const std::vector<const turn::Decider*> order = productionOrder(adapter, *engines.laya);
  engines.stack = std::make_unique<turn::FirstOf>(order);
  adapter.flow().useDecider(*engines.stack);
  adapter.flow().useWitnesses(order);
}

void installExtractor(const TurnEngineInput& input, TurnEngines& engines)
{
  const turn::BundleLoader bundle({.dir = input.extract.dir, .kind = turn::BundleKind::Extractor});
  if (!bundle.valid()) {
    engines.extractError = bundle.error();
    LOG_WARN << "argus-llm: the GLiNER extractor is off: " << engines.extractError
             << "; the turn fills its text slots with the rule tier";
    return;
  }
  engines.gliner = std::make_unique<turn::GlinerExtractor>(turn::GlinerExtractorInput{
      .model = turn::openGlinerModel(bundle, input.extract.options),
      .fallback = &input.adapter.ruleText(),
      .thresholds = bundle.thresholds()});
  input.adapter.flow().useText(*engines.gliner);
}
}

std::vector<const turn::Decider*> productionOrder(const LfmAdapter& adapter, const turn::Decider& laya)
{
  return {&adapter.ruleDecider(), &adapter.triggerDecider(), &laya};
}

int layaSessionThreads(int configured)
{
  if (configured > 0)
    return configured;
  return std::clamp(kLayaThreads, 1, std::max(1, ThreadBudget::hardwareThreads()));
}

std::string TurnEngines::pipeline() const
{
  std::string out = laya != nullptr ? "rules+triggers+laya" : "rules+triggers";
  out += gliner != nullptr ? "+gliner" : "+rule-text";
  return out;
}

TurnEngines installTurnEngines(const TurnEngineInput& input)
{
  TurnEngines engines;
  installDecider(input, engines);
  installExtractor(input, engines);
  return engines;
}
