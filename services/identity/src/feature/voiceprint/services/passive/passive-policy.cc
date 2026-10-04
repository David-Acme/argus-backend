#include "passive-policy.hxx"

#include <algorithm>
#include <cmath>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <set>
#include <utility>

namespace
{

constexpr int64_t kSecondsPerDay = 86400;
constexpr float kMadToSigma = 1.4826F;
constexpr float kOutlierSigmas = 3.0F;
constexpr float kMinOutlierGap = 0.15F;
constexpr int kClusterPasses = 3;

float median(std::vector<float> values)
{
  if (values.empty())
    return 0.0F;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::ranges::nth_element(values, middle);
  if (values.size() % 2 == 1)
    return *middle;
  const float upper = *middle;
  const float lower = *std::max_element(values.begin(), middle);
  return (lower + upper) / 2.0F;
}

std::vector<std::vector<float>> pick(std::span<const std::vector<float>> all,
                                     std::span<const size_t> indices)
{
  std::vector<std::vector<float>> out;
  out.reserve(indices.size());
  for (const size_t index : indices)
    out.push_back(all[index]);
  return out;
}

std::vector<size_t> near(std::span<const std::vector<float>> all,
                         std::span<const float> anchor, float threshold)
{
  std::vector<size_t> out;
  for (size_t index = 0; index < all.size(); ++index)
    if (voice_vector::cosine(all[index], anchor) >= threshold)
      out.push_back(index);
  return out;
}

int occasionsOf(std::vector<int64_t> times, int64_t gap)
{
  if (times.empty())
    return 0;
  std::ranges::sort(times);
  int occasions = 1;
  for (size_t index = 1; index < times.size(); ++index)
    if (times[index] - times[index - 1] >= gap)
      ++occasions;
  return occasions;
}

int64_t floorDiv(int64_t value, int64_t divisor)
{
  const int64_t quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1
                                                                : quotient;
}

}

TurnVerdict PassivePolicy::judgeTurn(const TurnJudgeInput& input) const
{
  if (std::cmp_greater_equal(input.accepted.size(), config_.maxCallTurns))
    return TurnVerdict::CallFull;
  if (input.halvesScore && *input.halvesScore < config_.turnSplitThreshold)
    return TurnVerdict::MixedTurn;
  if (input.bestOther &&
      input.bestOther->score >= config_.otherSpeakerThreshold &&
      (!input.ownScore || input.bestOther->score >= *input.ownScore))
    return TurnVerdict::OtherSpeaker;
  if (!input.accepted.empty() &&
      voice_vector::cosine(input.embedding,
                           voice_vector::centroid(input.accepted)) <
          config_.callConsistency)
    return TurnVerdict::Drift;
  return TurnVerdict::Accepted;
}

CallJudgement PassivePolicy::judgeCall(const CallJudgeInput& input) const
{
  if (input.tainted)
    return {.verdict = CallVerdict::Tainted, .centroid = {}};
  if (std::cmp_less(input.turns.size(), config_.minCallTurns))
    return {.verdict = CallVerdict::TooFewTurns, .centroid = {}};
  if (input.speechSeconds < config_.minCallSpeechSeconds)
    return {.verdict = CallVerdict::TooLittleSpeech, .centroid = {}};
  for (size_t index = 0; index < input.turns.size(); ++index) {
    std::vector<std::vector<float>> others;
    others.reserve(input.turns.size() - 1);
    for (size_t other = 0; other < input.turns.size(); ++other)
      if (other != index)
        others.push_back(input.turns[other]);
    if (voice_vector::cosine(input.turns[index],
                             voice_vector::centroid(others)) <
        config_.callConsistency)
      return {.verdict = CallVerdict::Inconsistent, .centroid = {}};
  }
  return {.verdict = CallVerdict::Usable,
          .centroid = voice_vector::centroid(input.turns)};
}

VoiceCluster PassivePolicy::dominantCluster(
    std::span<const std::vector<float>> embeddings) const
{
  if (embeddings.empty())
    return {};
  size_t seed = 0;
  size_t bestSupport = 0;
  for (size_t index = 0; index < embeddings.size(); ++index) {
    const size_t support =
        near(embeddings, embeddings[index], config_.clusterThreshold).size();
    if (support >= bestSupport) {
      bestSupport = support;
      seed = index;
    }
  }
  std::vector<size_t> members =
      near(embeddings, embeddings[seed], config_.clusterThreshold);
  std::vector<float> center =
      voice_vector::centroid(pick(embeddings, members));
  for (int pass = 0; pass < kClusterPasses; ++pass) {
    auto next = near(embeddings, center, config_.clusterThreshold);
    if (next.empty() || next == members)
      break;
    members = std::move(next);
    center = voice_vector::centroid(pick(embeddings, members));
  }
  if (members.empty())
    return {.members = {seed},
            .centroid = voice_vector::normalized(embeddings[seed])};
  return {.members = std::move(members), .centroid = std::move(center)};
}

LinkDecision PassivePolicy::evaluateLink(const LinkInput& input) const
{
  LinkDecision decision;
  if (input.samples.empty())
    return decision;

  std::vector<std::vector<float>> embeddings;
  embeddings.reserve(input.samples.size());
  for (const auto& sample : input.samples)
    embeddings.push_back(sample.embedding);
  auto cluster = dominantCluster(embeddings);

  std::vector<int64_t> ownTimes;
  std::set<int64_t> ownDays;
  for (const size_t index : cluster.members) {
    const auto& sample = input.samples[index];
    decision.members.push_back(sample.id);
    decision.speechSeconds += sample.speechSeconds;
    if (!sample.ownDevice)
      continue;
    ownTimes.push_back(sample.createdAt);
    ownDays.insert(
        floorDiv(sample.createdAt + input.utcOffsetSeconds, kSecondsPerDay));
  }
  decision.centroid = std::move(cluster.centroid);
  decision.dominance = static_cast<float>(cluster.members.size()) /
                       static_cast<float>(input.samples.size());
  decision.occasions = occasionsOf(std::move(ownTimes), config_.occasionGapSeconds);
  decision.days = static_cast<int>(ownDays.size());

  if (decision.dominance < config_.linkDominance)
    decision.verdict = LinkVerdict::NotDominant;
  else if (decision.occasions < config_.linkMinOccasions)
    decision.verdict = LinkVerdict::TooFewOccasions;
  else if (decision.days < config_.linkMinDays)
    decision.verdict = LinkVerdict::TooFewDays;
  else if (decision.speechSeconds < config_.linkMinSpeechSeconds)
    decision.verdict = LinkVerdict::TooLittleSpeech;
  else if (std::ranges::any_of(input.otherProfiles, [&](const auto& other) {
             return voice_vector::cosine(decision.centroid, other) >=
                    config_.otherSpeakerThreshold;
           }))
    decision.verdict = LinkVerdict::VoiceTaken;
  else
    decision.verdict = LinkVerdict::Link;
  return decision;
}

AdoptVerdict PassivePolicy::judgeAdoption(const AdoptInput& input) const
{
  if (input.bestOther &&
      input.bestOther->score >= config_.otherSpeakerThreshold &&
      input.bestOther->score >= input.ownScore)
    return AdoptVerdict::OtherBetter;
  const int known = input.deviceMatched + input.deviceConflicting;
  if (known >= config_.deviceConflictMinCalls &&
      static_cast<float>(input.deviceConflicting) / static_cast<float>(known) >
          config_.deviceConflictLimit)
    return AdoptVerdict::DeviceConflicted;
  const float margin = input.sharedDevice ? config_.sharedDeviceMargin : 0.0F;
  if (input.ownScore < config_.adoptThreshold + margin)
    return AdoptVerdict::Weak;
  return AdoptVerdict::Adopt;
}

RefreshResult PassivePolicy::refresh(const RefreshInput& input) const
{
  RefreshResult result;
  if (input.reservoir.empty())
    return result;
  const std::vector<float> first = voice_vector::centroid(input.reservoir);
  std::vector<float> scores;
  scores.reserve(input.reservoir.size());
  for (const auto& sample : input.reservoir)
    scores.push_back(voice_vector::cosine(sample, first));
  const float center = median(scores);
  std::vector<float> deviations;
  deviations.reserve(scores.size());
  for (const float score : scores)
    deviations.push_back(std::fabs(score - center));
  const float spread = kMadToSigma * median(deviations);
  const float cutoff =
      std::max(config_.outlierFloor,
               std::min(center - kOutlierSigmas * spread,
                        center - kMinOutlierGap));

  std::vector<std::vector<float>> kept;
  for (size_t index = 0; index < input.reservoir.size(); ++index)
    if (scores[index] >= cutoff)
      kept.push_back(input.reservoir[index]);
  result.kept = static_cast<int>(kept.size());
  result.dropped = static_cast<int>(input.reservoir.size() - kept.size());
  if (kept.empty())
    return result;

  const std::vector<float> fresh = voice_vector::centroid(kept);
  if (input.current.empty()) {
    result.applied = true;
    result.centroid = fresh;
    return result;
  }
  if (voice_vector::cosine(fresh, input.current) < config_.refreshMinAgreement)
    return result;
  const std::vector<float> current = voice_vector::normalized(input.current);
  std::vector<float> blended(current.size(), 0.0F);
  for (size_t index = 0; index < blended.size() && index < fresh.size(); ++index)
    blended[index] = (1.0F - config_.refreshBlend) * current[index] +
                     config_.refreshBlend * fresh[index];
  result.applied = true;
  result.centroid = voice_vector::normalized(blended);
  return result;
}
