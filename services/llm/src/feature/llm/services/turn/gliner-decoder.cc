#include "gliner-decoder.hxx"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace turn
{

namespace
{
bool overlaps(const GlinerSpan& left, const GlinerSpan& right)
{
  return left.start < right.end && right.start < left.end;
}

bool rankedBefore(const GlinerSpan& left, const GlinerSpan& right)
{
  if (left.probability != right.probability)
    return left.probability > right.probability;
  if (left.start != right.start)
    return left.start < right.start;
  if (left.end != right.end)
    return left.end < right.end;
  return left.query < right.query;
}

std::vector<GlinerSpan> resolve(const std::vector<GlinerSpan>& candidates, GlinerOverlap overlap)
{
  if (overlap == GlinerOverlap::Allow)
    return candidates;
  std::vector<GlinerSpan> ordered = candidates;
  std::ranges::sort(ordered, rankedBefore);
  std::vector<GlinerSpan> kept;
  for (const GlinerSpan& candidate : ordered) {
    const auto clashing = std::ranges::find_if(kept, [&candidate](const GlinerSpan& other) {
      return overlaps(candidate, other);
    });
    if (clashing == kept.end()) {
      kept.push_back(candidate);
      continue;
    }
    if (overlap == GlinerOverlap::Longest) {
      const std::int32_t width = candidate.end - candidate.start;
      const std::int32_t other = clashing->end - clashing->start;
      if (width > other)
        *clashing = candidate;
    }
  }
  return kept;
}
}

float glinerSigmoid(float value)
{
  if (value >= 0.0F)
    return 1.0F / (1.0F + std::exp(-value));
  const float exponential = std::exp(value);
  return exponential / (1.0F + exponential);
}

std::vector<GlinerSpan> glinerDecode(const GlinerDecodeInput& input)
{
  std::unordered_map<std::int32_t, std::vector<GlinerSpan>> byQuery;
  for (const GlinerCandidate& candidate : input.candidates) {
    if (candidate.end - candidate.start > input.maxWidth || candidate.end <= candidate.start)
      continue;
    const float threshold =
        candidate.query >= 0 && static_cast<std::size_t>(candidate.query) < input.queryThresholds.size()
            ? input.queryThresholds[static_cast<std::size_t>(candidate.query)]
            : input.defaultThreshold;
    const float probability = glinerSigmoid(candidate.logit);
    if (probability < threshold)
      continue;
    byQuery[candidate.query].push_back({.query = candidate.query,
                                        .start = candidate.start,
                                        .end = candidate.end,
                                        .probability = probability});
  }
  std::vector<GlinerSpan> resolved;
  for (const auto& [query, candidates] : byQuery) {
    const std::vector<GlinerSpan> kept = resolve(candidates, input.overlap);
    resolved.insert(resolved.end(), kept.begin(), kept.end());
  }
  std::ranges::sort(resolved, rankedBefore);
  return resolved;
}

GlinerOffsets glinerCharacterOffsets(const GlinerSpan& span,
                                     std::span<const std::int32_t> startMappings,
                                     std::span<const std::int32_t> endMappings)
{
  if (span.end <= span.start || span.start < 0 || static_cast<std::size_t>(span.end) > endMappings.size() ||
      static_cast<std::size_t>(span.start) >= startMappings.size())
    return {};
  return {.begin = startMappings[static_cast<std::size_t>(span.start)],
          .end = endMappings[static_cast<std::size_t>(span.end) - 1]};
}

}
