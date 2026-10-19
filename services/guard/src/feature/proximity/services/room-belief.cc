#include "room-belief.hxx"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kNegligible = 1e-3;
}

double evidenceWeight(const RoomEvidence& evidence, const RoomBeliefConfig& config)
{
  const double strength = std::clamp(evidence.strength, 0.0, 1.0);
  switch (evidence.kind) {
    case ProximityEvidenceKind::Face:
      return config.weightFace * strength * (evidence.night ? config.nightFaceFactor : 1.0);
    case ProximityEvidenceKind::RadarBox:
      return config.weightRadarBox * strength;
    case ProximityEvidenceKind::Voice:
      return config.weightVoice * strength;
    case ProximityEvidenceKind::Ble:
      return config.weightBle * strength;
  }
  return 0.0;
}

double decayFactor(int64_t elapsedMs, int64_t halfLifeMs)
{
  if (elapsedMs <= 0)
    return 1.0;
  if (halfLifeMs <= 0)
    return 0.0;
  return std::exp2(-static_cast<double>(elapsedMs) / static_cast<double>(halfLifeMs));
}

RoomBelief::RoomBelief(RoomBeliefConfig config) : config_(config) {}

void RoomBelief::configure(const RoomBeliefConfig& config)
{
  std::scoped_lock lock(mutex_);
  config_ = config;
}

void RoomBelief::decayTo(Person& person, int64_t atMs) const
{
  const double factor = decayFactor(atMs - person.scoredAtMs, config_.halfLifeMs);
  if (atMs > person.scoredAtMs)
    person.scoredAtMs = atMs;
  for (auto it = person.rooms.begin(); it != person.rooms.end();) {
    it->second.score *= factor;
    it->second.strongestWeight *= factor;
    if (it->second.score < kNegligible)
      it = person.rooms.erase(it);
    else
      ++it;
  }
}

void RoomBelief::settle(Person& person, int64_t atMs) const
{
  if (person.rooms.empty()) {
    person.current.clear();
    person.challenger.clear();
    return;
  }
  const auto leader = std::ranges::max_element(person.rooms, {}, [](const auto& entry) { return entry.second.score; });
  double total = 0.0;
  for (const auto& [room, score] : person.rooms)
    total += score.score;
  if (person.current.empty() || !person.rooms.contains(person.current)) {
    person.current = leader->first;
    person.currentSinceMs = atMs;
    person.challenger.clear();
    return;
  }
  if (leader->first == person.current) {
    person.challenger.clear();
    return;
  }
  const double lead = (leader->second.score - person.rooms.at(person.current).score) / std::max(total, kNegligible);
  if (lead < config_.switchMargin) {
    person.challenger.clear();
    return;
  }
  if (person.challenger != leader->first) {
    person.challenger = leader->first;
    person.challengerSinceMs = atMs;
  }
  if (atMs - person.challengerSinceMs >= config_.switchHoldMs) {
    person.current = leader->first;
    person.currentSinceMs = atMs;
    person.challenger.clear();
  }
}

void RoomBelief::observe(const RoomEvidence& evidence)
{
  if (evidence.userId <= 0 || evidence.room.empty())
    return;
  std::scoped_lock lock(mutex_);
  const double weight = evidenceWeight(evidence, config_);
  if (weight <= 0.0)
    return;
  Person& person = people_[evidence.userId];
  decayTo(person, evidence.atMs);
  const double age = decayFactor(person.scoredAtMs - evidence.atMs, config_.halfLifeMs);
  RoomScore& room = person.rooms[evidence.room];
  room.score += weight * age;
  if (weight * age >= room.strongestWeight) {
    room.strongestWeight = weight * age;
    room.strongest = evidence.kind;
  }
  person.updatedMs = std::max(person.updatedMs, evidence.atMs);
  settle(person, person.scoredAtMs);
}

std::optional<RoomVerdict> RoomBelief::verdictOf(int64_t userId, Person& person, int64_t atMs) const
{
  decayTo(person, atMs);
  settle(person, atMs);
  if (person.current.empty() || atMs - person.updatedMs > config_.forgetMs)
    return std::nullopt;
  double total = 0.0;
  for (const auto& [room, score] : person.rooms)
    total += score.score;
  const RoomScore& current = person.rooms.at(person.current);
  return RoomVerdict{.userId = userId,
                     .room = person.current,
                     .confidence = total > 0.0 ? current.score / total : 0.0,
                     .sinceMs = person.currentSinceMs,
                     .updatedMs = person.updatedMs,
                     .strongest = current.strongest};
}

std::optional<RoomVerdict> RoomBelief::verdict(const RoomBeliefQuery& query)
{
  std::scoped_lock lock(mutex_);
  const auto found = people_.find(query.userId);
  if (found == people_.end())
    return std::nullopt;
  return verdictOf(query.userId, found->second, query.nowMs);
}

std::vector<RoomVerdict> RoomBelief::all(int64_t nowMs)
{
  std::vector<RoomVerdict> verdicts;
  std::scoped_lock lock(mutex_);
  for (auto it = people_.begin(); it != people_.end();) {
    if (nowMs - it->second.updatedMs > config_.forgetMs) {
      it = people_.erase(it);
      continue;
    }
    if (auto verdict = verdictOf(it->first, it->second, nowMs))
      verdicts.push_back(std::move(*verdict));
    ++it;
  }
  std::ranges::sort(verdicts, {}, &RoomVerdict::userId);
  return verdicts;
}

void RoomBelief::forget(int64_t userId)
{
  std::scoped_lock lock(mutex_);
  people_.erase(userId);
}
