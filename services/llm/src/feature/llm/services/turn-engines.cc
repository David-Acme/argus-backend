#include "turn-engines.hxx"

#include <feature/llm/services/lfm-adapter.hxx>

#include <trantor/utils/Logger.h>

#include <optional>
#include <vector>

namespace
{
void installDecider(const TurnEngineInput& input, TurnEngines& engines)
{
  const turn::BundleLoader bundle({.dir = input.decide.dir, .pin = input.decide.pin});
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
  LfmAdapter& adapter = input.adapter;
  engines.laya = std::make_unique<turn::LayaDecider>(turn::LayaDeciderInput{
      .model = turn::openLayaModel({.bundle = bundle, .options = input.decide.options, .decode = bundle.decode()}),
      .fallback = &adapter.triggerDecider(),
      .policy = bundle.policy(),
      .confidence = bundle.confidenceCalibration(),
      .now = bundle.nowCalibration()});
  engines.stack = std::make_unique<turn::FirstOf>(std::vector<const turn::Decider*>{&adapter.ruleDecider(), engines.laya.get()});
  adapter.flow().useDecider(*engines.stack);
  adapter.flow().useWitnesses({&adapter.ruleDecider(), engines.laya.get()});
}

void installExtractor(const TurnEngineInput& input, TurnEngines& engines)
{
  const turn::BundleLoader bundle({.dir = input.extract.dir, .pin = input.extract.pin, .kind = turn::BundleKind::Extractor});
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

std::string TurnEngines::pipeline() const
{
  std::string out = laya != nullptr ? "rules+laya" : "rules+triggers";
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
