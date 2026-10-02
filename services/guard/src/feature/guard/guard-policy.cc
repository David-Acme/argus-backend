#include "guard-policy.hxx"

#include <text/base64.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace guard_policy
{

namespace
{

GuardDanger companionRelaxation(const GuardContext& context)
{
  if (context.mode == GuardMode::Away || context.mode == GuardMode::Armed)
    return GuardDanger::None;
  if (context.inAlertZone || context.atNight || context.escalated)
    return GuardDanger::None;
  if (context.accompaniedByResident)
    return GuardDanger::Low;
  if (context.accompaniedByGuest)
    return GuardDanger::Medium;
  return GuardDanger::None;
}

GuardDanger hardFloor(const GuardContext& context)
{
  GuardDanger floor = GuardDanger::None;
  if (context.mode == GuardMode::Away || context.mode == GuardMode::Armed)
    floor = GuardDanger::Critical;
  if (context.inAlertZone)
    floor = context.staffOnly ? GuardDanger::High : GuardDanger::Critical;
  if (context.mode == GuardMode::Night)
    floor = std::max(floor, GuardDanger::High);
  if ((context.atNight && !context.staffOnly) || context.escalated)
    floor = std::max(floor, GuardDanger::High);
  if (context.strangerCount >= 2)
    floor = std::max(floor, GuardDanger::High);
  if (context.visitCount >= 3)
    floor = std::max(floor, GuardDanger::High);
  return floor;
}

}

GuardDanger evaluate(const GuardContext& context)
{
  if (!context.hasUnknown) {
    if (context.hasKnown)
      return GuardDanger::None;
    return GuardDanger::Low;
  }

  if (context.publicPresent)
    return context.inAlertZone ? GuardDanger::Medium : GuardDanger::Low;

  GuardDanger soft =
      context.expectedGuest ? GuardDanger::Low : GuardDanger::Medium;
  const GuardDanger companion = companionRelaxation(context);
  if (companion != GuardDanger::None && companion < soft)
    soft = companion;

  GuardDanger danger = std::max(soft, hardFloor(context));

  if (context.severity == "critical")
    danger = std::max(danger, GuardDanger::High);
  else if (context.severity == "warning")
    danger = std::max(danger, GuardDanger::Medium);

  if (context.expectedGuest)
    danger = std::min(danger, GuardDanger::Medium);
  return danger;
}

GuardDeterrence deterrence(const GuardDeterrenceInput& input)
{
  const bool peoplePresent = input.publicPresent || input.staffOnly ||
                             input.mode == GuardMode::Home ||
                             input.mode == GuardMode::Night;
  if (input.publicPresent || input.staffOnly ||
      (input.weapon && peoplePresent))
    return {.voice = false, .alarm = false};

  const bool critical = input.danger == GuardDanger::Critical;
  GuardDeterrence result{.voice = dangerRank(input.danger) >=
                                  dangerRank(GuardDanger::High),
                         .alarm = false};
  switch (input.mode) {
    case GuardMode::Home:
      result.alarm = false;
      break;
    case GuardMode::Night:
    case GuardMode::Armed:
      result.alarm = critical;
      break;
    case GuardMode::Away:
      result.alarm = critical && (input.weapon || input.inAlertZone ||
                                  input.encounterChecks >= 2);
      break;
  }
  return result;
}

namespace
{

GuardPersonSignals parsePerson(const Json::Value& object)
{
  GuardPersonSignals person;
  person.trackId = object.get("trackId", 0).asInt64();
  const int64_t personId = object.get("personId", 0).asInt64();
  const std::string legacyIdentity = object.get("identity", "").asString();
  const std::string stateName = object.get("identityState", "").asString();
  const IdentityState state = identityStateFromString(stateName);
  person.known = legacyIdentity == "known" && personId > 0 &&
                 (stateName.empty() || state == IdentityState::Known);
  person.personId = personId;
  person.identityState = person.known ? IdentityState::Known : state;
  person.identityAvailable = object.isMember("identityState");
  person.identityConfidence =
      object.get("identityConfidence", 0.0).asFloat();
  person.identifyAttempts = object.get("identifyAttempts", 0).asInt();
  person.scoreMedian = object.get("scoreMedian", 0.0).asDouble();
  person.scoreSamples = object.get("scoreSamples", 0).asInt();
  person.area = object["bbox"].get("w", 0.0).asDouble() *
                object["bbox"].get("h", 0.0).asDouble();
  person.zoneWindows = object.get("zoneWindows", 0).asInt();
  person.trackWindows = object.get("trackWindows", 0).asInt();
  person.areaSpread = object.get("areaSpread", 1.0).asDouble();
  person.zoneKind = object.get("zoneKind", "").asString();
  person.signature = object.get("signature", "").asString();
  person.observationId = object.get("observationId", "").asString();
  person.firstSeenMs = object.get("firstSeenMs", 0).asInt64();
  person.dwellMs = object.get("dwellMs", 0).asInt64();
  return person;
}

void adoptSubject(GuardEventSignals& signals, const GuardPersonSignals& person)
{
  signals.personId = person.personId;
  signals.trackId = person.trackId;
  signals.identityState = person.identityState;
  signals.identityAvailable = person.identityAvailable;
  signals.identityConfidence = person.identityConfidence;
  signals.identifyAttempts = person.identifyAttempts;
  signals.scoreMedian = person.scoreMedian;
  signals.scoreSamples = person.scoreSamples;
  signals.zoneWindows = person.zoneWindows;
  signals.trackWindows = person.trackWindows;
  signals.areaSpread = person.areaSpread;
  signals.zoneKind = person.zoneKind;
  signals.observationId = person.observationId;
  signals.firstSeenMs = person.firstSeenMs;
  signals.dwellMs = person.dwellMs;
  if (!person.signature.empty())
    signals.signature = person.signature;
}

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
  if (objects.isArray()) {
    for (const auto& object : objects) {
      if (object.get("class", "").asString() != "person")
        continue;
      personPresent = true;
      signals.persons.push_back(parsePerson(object));
    }
  }

  for (const auto& person : signals.persons) {
    if (person.known)
      ++signals.knownCount;
    else
      ++signals.unknownCount;
    if (!person.known && person.identityAvailable &&
        person.identityState == IdentityState::Unrecognized)
      ++signals.strangerCount;
    signals.viewScore = std::max(signals.viewScore, person.area);
  }
  signals.hasKnown = signals.knownCount > 0;
  signals.hasUnknown = signals.unknownCount > 0;

  const GuardPersonSignals* subject = nullptr;
  const GuardPersonSignals* primaryUnknown = nullptr;
  const GuardPersonSignals* firstUnknown = nullptr;
  const GuardPersonSignals* primaryPerson = nullptr;
  const GuardPersonSignals* largest = nullptr;
  for (const auto& person : signals.persons) {
    if (!person.known) {
      if (firstUnknown == nullptr)
        firstUnknown = &person;
      if (primaryTrackId > 0 && person.trackId == primaryTrackId &&
          primaryUnknown == nullptr)
        primaryUnknown = &person;
    }
    if (primaryTrackId > 0 && person.trackId == primaryTrackId &&
        primaryPerson == nullptr)
      primaryPerson = &person;
    if (largest == nullptr || person.area > largest->area)
      largest = &person;
  }

  if (primaryUnknown != nullptr)
    subject = primaryUnknown;
  else if (firstUnknown != nullptr)
    subject = firstUnknown;
  else if (primaryPerson != nullptr)
    subject = primaryPerson;
  else
    subject = largest;

  if (subject != nullptr) {
    adoptSubject(signals, *subject);
    if (subject->known)
      signals.knownPersonId = subject->personId;
  }
  if (signals.knownPersonId == 0) {
    for (const auto& person : signals.persons) {
      if (person.known) {
        signals.knownPersonId = person.personId;
        break;
      }
    }
  }

  if (signals.trackId == 0)
    signals.trackId = primaryTrackId;
  if (signals.dwellMs == 0)
    signals.dwellMs = event.get("dwellMs", 0).asInt64();
  signals.publishedAtMs = event.get("publishedAt", 0).asInt64();
  if (signals.firstSeenMs == 0)
    signals.firstSeenMs = event.get("capturedAt", 0).asInt64();
  signals.night = event.get("night", false).asBool();
  if (signals.rule.rfind("person", 0) == 0)
    personPresent = true;
  if (signals.persons.empty())
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

double decayBaseline(double stored, int64_t elapsedS)
{
  if (stored <= 0.0)
    return 0.0;
  const double elapsed =
      static_cast<double>(std::max<int64_t>(0, elapsedS));
  return stored *
         std::exp(-std::numbers::ln2 * elapsed / kBaselineHalfLifeS);
}

double baselineNovelty(double decayed)
{
  return 1.0 / (1.0 + std::max(0.0, decayed));
}

}
