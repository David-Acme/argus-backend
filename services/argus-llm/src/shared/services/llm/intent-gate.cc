#include "intent-gate.hxx"

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/extract/extract-contracts.hxx>
#include <shared/services/extract/temporal-resolver.hxx>

#include <drogon/drogon.h>

namespace
{

constexpr const char* kDefaultModelPath = "models/intent/intent.bin";

std::string modelPath()
{
  const std::string configured = ConfigService::getString("intent.model_file");
  return configured.empty() ? kDefaultModelPath : configured;
}

// The rubric the taxonomy fixes: atemporal or recurring is a fact, a single
// future instant is a reminder.
bool atemporalOrRecurring(const std::string& text, const std::string& lang)
{
  static const TemporalResolver resolver;
  extract::TemporalValue when;
  resolver.resolve(lang.empty() ? "es" : lang,
                   TemporalResolver::normalize(text), when);
  return when.kind == extract::TemporalKind::None ||
         when.recur != extract::Recurrence::None;
}

} // namespace

IntentGate::IntentGate()
    : classifier_(modelPath()),
      router_({.catalog = catalog_,
               .model = classifier_,
               .recurrent = atemporalOrRecurring})
{
  catalog_.build();
  if (classifier_.isLoaded())
    LOG_INFO << "IntentGate: intent model loaded from " << modelPath();
  else
    LOG_WARN << "IntentGate: no intent model at " << modelPath()
             << "; every turn stays on the LLM's tool calling";
}
