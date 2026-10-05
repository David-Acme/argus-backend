#include "visitor-policy.hxx"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
std::optional<PoolNeighbour> best(std::span<const PoolNeighbour> neighbours,
                                  bool household,
                                  std::optional<int64_t> skip = std::nullopt)
{
  std::optional<PoolNeighbour> found;
  for (const auto& neighbour : neighbours) {
    if ((neighbour.pool == PersonPool::Household) != household)
      continue;
    if (skip && neighbour.personId == *skip)
      continue;
    if (!found || neighbour.score > found->score)
      found = neighbour;
  }
  return found;
}
}

VisitorThresholds visitor_policy::defaults()
{
  return {.householdMatch = 0.50F,
          .householdGuard = 0.30F,
          .visitorMatch = 0.55F,
          .visitorNewCeiling = 0.35F,
          .visitorMargin = 0.08F,
          .sampleConsistency = 0.35F,
          .duplicateSimilarity = 0.92F,
          .matchGate = {.minDetectorScore = 0.80F,
                        .minInterOcularPx = 12.0F,
                        .maxYaw = 0.35F,
                        .minPitch = 0.25F,
                        .maxPitch = 0.85F,
                        .minSharpness = 0.0F},
          .learnGate = {.minDetectorScore = 0.90F,
                        .minInterOcularPx = 16.0F,
                        .maxYaw = 0.25F,
                        .minPitch = 0.30F,
                        .maxPitch = 0.80F,
                        .minSharpness = 0.0F},
          .maxSamples = 8};
}

SightingDecision visitor_policy::decide(const DecideSightingInput& input)
{
  const VisitorThresholds& t = input.thresholds;
  if (face_quality::judge(input.quality, t.matchGate) !=
      FaceQualityVerdict::Accepted)
    return {.outcome = SightingOutcome::LowQuality};
  const bool learnable = face_quality::judge(input.quality, t.learnGate) ==
                         FaceQualityVerdict::Accepted;

  const auto household = best(input.neighbours, true);
  const auto visitor = input.recognitionEnabled
                           ? best(input.neighbours, false)
                           : std::nullopt;
  const float householdScore = household ? household->score : -1.0F;
  const float visitorScore = visitor ? visitor->score : -1.0F;

  if (household && householdScore >= t.householdMatch &&
      householdScore >= visitorScore)
    return {.outcome = SightingOutcome::Household,
            .personId = household->personId,
            .score = householdScore};
  if (!input.recognitionEnabled)
    return {.outcome = SightingOutcome::Disabled};
  if (householdScore >= t.householdGuard)
    return {.outcome = SightingOutcome::NearHousehold,
            .personId = 0,
            .score = householdScore};

  if (visitor && visitorScore >= t.visitorMatch) {
    const auto runnerUp = best(input.neighbours, false, visitor->personId);
    if (runnerUp && visitorScore - runnerUp->score < t.visitorMargin)
      return {.outcome = SightingOutcome::Ambiguous,
              .personId = visitor->personId,
              .score = visitorScore};
    return {.outcome = SightingOutcome::Visitor,
            .personId = visitor->personId,
            .score = visitorScore,
            .learn = learnable};
  }
  if (visitorScore >= t.visitorNewCeiling)
    return {.outcome = SightingOutcome::Uncertain,
            .personId = 0,
            .score = visitorScore};
  if (!learnable)
    return {.outcome = SightingOutcome::LowQuality};
  return {.outcome = SightingOutcome::NewVisitor,
          .personId = 0,
          .score = std::max(visitorScore, 0.0F),
          .learn = true};
}

float visitor_policy::median(std::span<const float> values)
{
  if (values.empty())
    return 0.0F;
  std::vector<float> sorted(values.begin(), values.end());
  std::ranges::sort(sorted);
  const std::size_t middle = sorted.size() / 2;
  if (sorted.size() % 2 == 1)
    return sorted[middle];
  return (sorted[middle - 1] + sorted[middle]) * 0.5F;
}

SampleAdmission visitor_policy::admit(const AdmitSampleInput& input)
{
  if (input.similarities.empty())
    return {.kind = SampleAdmissionKind::Add};
  if (median(input.similarities) < input.thresholds.sampleConsistency)
    return {.kind = SampleAdmissionKind::Outlier};
  const auto closest = std::ranges::max_element(input.similarities);
  const auto closestIndex =
      static_cast<std::size_t>(closest - input.similarities.begin());
  if (*closest >= input.thresholds.duplicateSimilarity &&
      closestIndex < input.existingQualities.size()) {
    if (input.quality > input.existingQualities[closestIndex])
      return {.kind = SampleAdmissionKind::Replace, .replaceIndex = closestIndex};
    return {.kind = SampleAdmissionKind::Skip};
  }
  if (std::cmp_less(input.existingQualities.size(), input.thresholds.maxSamples))
    return {.kind = SampleAdmissionKind::Add};
  const auto worst = std::ranges::min_element(input.existingQualities);
  if (worst == input.existingQualities.end() || input.quality <= *worst)
    return {.kind = SampleAdmissionKind::Skip};
  return {.kind = SampleAdmissionKind::Replace,
          .replaceIndex = static_cast<std::size_t>(
              worst - input.existingQualities.begin())};
}
