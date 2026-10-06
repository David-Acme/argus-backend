#include "decision-tally.hxx"

#include <trantor/utils/Logger.h>

namespace turn
{

void DecisionTally::note(const Decision& decision)
{
  const std::string_view family = decision.tool.substr(0, decision.tool.find('.'));
  LOG_INFO << "turn-decision decider=" << std::string(decision.decider) << " exact=" << (decision.exact ? 1 : 0)
           << " family=" << std::string(family) << " tool=" << std::string(decision.tool) << " lang=" << std::string(decision.lang)
           << " verdict=" << std::string(decision.verdict);
  const DecisionKey key{.decider = std::string(decision.decider),
                        .exact = decision.exact,
                        .family = std::string(family),
                        .lang = std::string(decision.lang),
                        .verdict = std::string(decision.verdict)};
  const std::scoped_lock lock(mutex_);
  ++counts_[key];
}

std::vector<DecisionCount> DecisionTally::snapshot() const
{
  const std::scoped_lock lock(mutex_);
  std::vector<DecisionCount> out;
  out.reserve(counts_.size());
  for (const auto& [key, count] : counts_)
    out.push_back({.key = key, .count = count});
  return out;
}

}
