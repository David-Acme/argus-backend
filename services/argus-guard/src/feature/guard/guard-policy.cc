#include "guard-policy.hxx"

#include <shared/utils/base64/base64.hxx>

#include <algorithm>
#include <cstdint>

namespace guard_policy
{

GuardDanger evaluate(const GuardContext& context)
{
  if (!context.hasUnknown) {
    if (context.hasKnown)
      return GuardDanger::None;
    return GuardDanger::Low;
  }

  GuardDanger danger = GuardDanger::Medium;
  if (context.expectedGuest)
    danger = GuardDanger::Low;
  else if (context.mode == GuardMode::Away || context.mode == GuardMode::Armed)
    danger = GuardDanger::Critical;
  else if (context.inAlertZone)
    danger = GuardDanger::Critical;
  else if (context.atNight || context.escalated)
    danger = GuardDanger::High;

  if (context.trustedCompanion && !context.inAlertZone && !context.atNight &&
      context.mode != GuardMode::Away)
    danger = GuardDanger::Low;

  if (context.visitCount >= 3 && danger < GuardDanger::High)
    danger = GuardDanger::High;

  if (context.severity == "critical" && danger < GuardDanger::High)
    danger = GuardDanger::High;
  else if (context.severity == "warning" && danger < GuardDanger::Medium)
    danger = GuardDanger::Medium;

  return danger;
}

GuardEventSignals parseObjectEvent(const Json::Value& event)
{
  GuardEventSignals signals;
  signals.cameraId = event.get("cameraId", 0).asInt64();
  signals.cameraName = event.get("cameraName", "").asString();
  signals.rule = event.get("rule", "").asString();
  signals.severity = event.get("severity", "").asString();
  signals.escalated = event.get("escalated", false).asBool();

  bool personPresent = false;
  const int64_t primaryTrackId = event.get("trackId", 0).asInt64();
  const Json::Value& objects = event["objects"];

  const Json::Value* primary = nullptr;
  const Json::Value* largest = nullptr;
  double largestArea = -1.0;
  if (objects.isArray()) {
    for (const auto& object : objects) {
      if (object.get("class", "").asString() != "person")
        continue;
      personPresent = true;
      const int64_t trackId = object.get("trackId", 0).asInt64();
      const double area = object["bbox"].get("w", 0.0).asDouble() *
                          object["bbox"].get("h", 0.0).asDouble();
      signals.viewScore = std::max(signals.viewScore, area);
      if (area > largestArea) {
        largestArea = area;
        largest = &object;
      }
      if (primaryTrackId > 0 && trackId == primaryTrackId)
        primary = &object;
    }
  }
  if (primary == nullptr)
    primary = largest;

  if (primary != nullptr) {
    const int64_t personId = primary->get("personId", 0).asInt64();
    const bool known =
        primary->get("identity", "").asString() == "known" && personId > 0;
    signals.hasKnown = known;
    signals.hasUnknown = !known;
    signals.personId = personId;
    signals.knownPersonId = known ? personId : 0;
    signals.trackId = primary->get("trackId", 0).asInt64();
    signals.firstSeenMs = primary->get("firstSeenMs", 0).asInt64();
    signals.dwellMs = primary->get("dwellMs", 0).asInt64();
    signals.observationId = primary->get("observationId", "").asString();
    signals.zoneKind = primary->get("zoneKind", "").asString();
    signals.identityConfidence =
        primary->get("identityConfidence", 0.0).asFloat();
    const std::string signature = primary->get("signature", "").asString();
    if (!signature.empty())
      signals.signature = signature;
  }
  if (signals.trackId == 0)
    signals.trackId = primaryTrackId;
  if (signals.dwellMs == 0)
    signals.dwellMs = event.get("dwellMs", 0).asInt64();
  signals.publishedAtMs = event.get("publishedAt", 0).asInt64();
  if (signals.firstSeenMs == 0)
    signals.firstSeenMs = event.get("capturedAt", 0).asInt64();
  if (signals.rule.rfind("person", 0) == 0)
    personPresent = true;
  if (primary == nullptr)
    signals.hasUnknown = personPresent;
  return signals;
}

double signatureSimilarity(const std::string& left, const std::string& right)
{
  const auto leftBytes = base64::decode(left);
  const auto rightBytes = base64::decode(right);
  if (!leftBytes || !rightBytes || leftBytes->empty() ||
      leftBytes->size() != rightBytes->size())
    return 0.0;

  int shared = 0;
  int leftTotal = 0;
  int rightTotal = 0;
  for (size_t i = 0; i < leftBytes->size(); ++i) {
    const int l = static_cast<uint8_t>((*leftBytes)[i]);
    const int r = static_cast<uint8_t>((*rightBytes)[i]);
    shared += std::min(l, r);
    leftTotal += l;
    rightTotal += r;
  }
  const int total = std::max(leftTotal, rightTotal);
  return total > 0 ? static_cast<double>(shared) / total : 0.0;
}

std::optional<int64_t> matchEncounter(const GuardEncounterMatchInput& input)
{
  std::optional<int64_t> best;
  double bestSimilarity = input.minSimilarity;
  std::optional<int64_t> continuity;
  int64_t continuitySeen = 0;
  for (const auto& candidate : input.candidates) {
    if (input.now - candidate.lastSeen > input.windowS)
      continue;
    if (input.personId > 0) {
      if (candidate.personId == input.personId)
        return candidate.id;
      continue;
    }
    if (!input.signature.empty()) {
      const double similarity =
          signatureSimilarity(input.signature, candidate.signature);
      if (similarity >= bestSimilarity) {
        bestSimilarity = similarity;
        best = candidate.id;
      }
    }
    if (input.cameraId > 0 && candidate.lastCameraId == input.cameraId &&
        input.now - candidate.lastSeen <= input.continuityWindowS &&
        candidate.lastSeen >= continuitySeen) {
      continuitySeen = candidate.lastSeen;
      continuity = candidate.id;
    }
  }
  return best ? best : continuity;
}

std::string pickGreeting(int64_t seed, const std::vector<std::string>& variants)
{
  if (variants.empty())
    return {};
  const size_t index =
      static_cast<size_t>(seed < 0 ? -seed : seed) % variants.size();
  return variants[index];
}

GuardMode modeFromString(const std::string& value)
{
  return guardModeFromString(value);
}

std::string modeToString(GuardMode mode)
{
  return guardModeToString(mode);
}

std::string dangerToString(GuardDanger danger)
{
  return guardDangerToString(danger);
}

GuardDanger dangerFromString(const std::string& value)
{
  return guardDangerFromString(value);
}

int dangerRank(GuardDanger danger)
{
  return guardDangerRank(danger);
}

} // namespace guard_policy
