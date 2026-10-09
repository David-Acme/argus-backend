#include "gliner-decoder.hxx"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace turn
{

namespace
{
struct Ranked
{
  float score{0.0F};
  std::int32_t start{0};
  std::int32_t end{0};
  std::size_t order{0};
};

bool ranksBefore(const Ranked& left, const Ranked& right)
{
  if (left.score != right.score)
    return left.score > right.score;
  if (left.start != right.start)
    return left.start < right.start;
  if (left.end != right.end)
    return left.end < right.end;
  return left.order < right.order;
}

bool sameBoundaries(const Ranked& left, const Ranked& right)
{
  return left.start == right.start && left.end == right.end;
}

std::vector<Ranked> collapse(const std::vector<Ranked>& spans)
{
  std::vector<Ranked> ranked = spans;
  std::ranges::sort(ranked, ranksBefore);
  std::vector<Ranked> distinct;
  distinct.reserve(ranked.size());
  for (const Ranked& span : ranked) {
    const bool seen = std::ranges::any_of(distinct, [&span](const Ranked& other) {
      return sameBoundaries(span, other);
    });
    if (!seen)
      distinct.push_back(span);
  }
  return distinct;
}

std::vector<Ranked> longestOnly(const std::vector<Ranked>& distinct)
{
  std::vector<Ranked> kept;
  kept.reserve(distinct.size());
  for (const Ranked& span : distinct) {
    const bool contained = std::ranges::any_of(distinct, [&span](const Ranked& other) {
      return (other.start < span.start || span.end < other.end) && other.start <= span.start && span.end <= other.end;
    });
    if (!contained)
      kept.push_back(span);
  }
  return kept;
}

std::vector<Ranked> selectionKey(const std::vector<Ranked>& byEnd, const std::vector<std::size_t>& selection)
{
  std::vector<Ranked> rows;
  rows.reserve(selection.size());
  for (const std::size_t at : selection)
    rows.push_back(byEnd[at]);
  std::ranges::sort(rows, ranksBefore);
  return rows;
}

bool keyBefore(const std::vector<Ranked>& left, const std::vector<Ranked>& right)
{
  const std::size_t shared = std::min(left.size(), right.size());
  for (std::size_t at = 0; at < shared; ++at) {
    if (ranksBefore(left[at], right[at]))
      return true;
    if (ranksBefore(right[at], left[at]))
      return false;
  }
  return left.size() < right.size();
}

std::vector<Ranked> heaviestRun(const std::vector<Ranked>& distinct)
{
  std::vector<Ranked> byEnd = distinct;
  std::ranges::sort(byEnd, [](const Ranked& left, const Ranked& right) {
    if (left.end != right.end)
      return left.end < right.end;
    if (left.start != right.start)
      return left.start < right.start;
    if (left.score != right.score)
      return left.score > right.score;
    return left.order < right.order;
  });
  std::vector<std::int32_t> ends;
  ends.reserve(byEnd.size());
  for (const Ranked& span : byEnd)
    ends.push_back(span.end);
  std::vector<std::ptrdiff_t> predecessors;
  predecessors.reserve(byEnd.size());
  for (std::size_t at = 0; at < byEnd.size(); ++at) {
    const auto limit = ends.begin() + static_cast<std::ptrdiff_t>(at);
    const auto found = std::upper_bound(ends.begin(), limit, byEnd[at].start);
    predecessors.push_back(static_cast<std::ptrdiff_t>(found - ends.begin()) - 1);
  }
  struct Best
  {
    double score{0.0};
    std::vector<std::size_t> selection;
  };
  std::vector<Best> best;
  best.reserve(byEnd.size() + 1);
  best.push_back({.score = 0.0, .selection = {}});
  for (std::size_t at = 0; at < byEnd.size(); ++at) {
    Best with = best[static_cast<std::size_t>(predecessors[at] + 1)];
    with.score += static_cast<double>(byEnd[at].score);
    with.selection.push_back(at);
    if (with.score > best[at].score)
      best.push_back(std::move(with));
    else if (with.score < best[at].score)
      best.push_back(best[at]);
    else if (with.selection.size() > best[at].selection.size())
      best.push_back(std::move(with));
    else if (with.selection.size() < best[at].selection.size())
      best.push_back(best[at]);
    else
      best.push_back(keyBefore(selectionKey(byEnd, with.selection), selectionKey(byEnd, best[at].selection))
                         ? std::move(with)
                         : best[at]);
  }
  std::vector<Ranked> selected;
  selected.reserve(best.back().selection.size());
  for (const std::size_t index : best.back().selection)
    selected.push_back(byEnd[index]);
  std::ranges::sort(selected, ranksBefore);
  return selected;
}

std::vector<Ranked> resolve(const std::vector<Ranked>& candidates, GlinerOverlap overlap)
{
  const std::vector<Ranked> distinct = collapse(candidates);
  if (overlap == GlinerOverlap::Allow)
    return distinct;
  if (overlap == GlinerOverlap::Longest)
    return longestOnly(distinct);
  return heaviestRun(distinct);
}

bool decodedBefore(const GlinerDecodedSpan& left, const GlinerDecodedSpan& right)
{
  if (left.probability != right.probability)
    return left.probability > right.probability;
  if (left.start != right.start)
    return left.start < right.start;
  if (left.end != right.end)
    return left.end < right.end;
  return left.query < right.query;
}
}

float glinerSigmoid(float value)
{
  if (value >= 0.0F)
    return 1.0F / (1.0F + std::exp(-value));
  const float exponential = std::exp(value);
  return exponential / (1.0F + exponential);
}

std::vector<GlinerDecodedSpan> glinerDecode(const GlinerDecodeInput& input)
{
  std::unordered_map<std::int32_t, std::vector<Ranked>> byQuery;
  for (const GlinerCandidate& candidate : input.candidates) {
    if (candidate.end - candidate.start > input.maxWidth)
      continue;
    const float threshold =
        candidate.query >= 0 && static_cast<std::size_t>(candidate.query) < input.queryThresholds.size()
            ? input.queryThresholds[static_cast<std::size_t>(candidate.query)]
            : input.defaultThreshold;
    const float probability = glinerSigmoid(candidate.logit);
    if (probability < threshold)
      continue;
    std::vector<Ranked>& pool = byQuery[candidate.query];
    pool.push_back({.score = probability, .start = candidate.start, .end = candidate.end, .order = pool.size()});
  }
  std::vector<GlinerDecodedSpan> resolved;
  for (const auto& [query, candidates] : byQuery) {
    for (const Ranked& kept : resolve(candidates, input.overlap)) {
      resolved.push_back({.query = query,
                          .start = kept.start,
                          .end = kept.end,
                          .probability = kept.score});
    }
  }
  std::ranges::sort(resolved, decodedBefore);
  return resolved;
}

GlinerOffsets glinerCharacterOffsets(const GlinerDecodedSpan& span,
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
