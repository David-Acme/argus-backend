#include "guard-risk.hxx"

#include <algorithm>

namespace
{
constexpr int kRaiseThreshold = 3;
constexpr int kSoftThreshold = 1;
} // namespace

bool guard_risk::evidenceFromTag(const std::string& tag, RiskEvidence& evidence)
{
  if (tag == "concealed_face" || tag == "masked") {
    evidence = RiskEvidence::ConcealedFace;
    return true;
  }
  if (tag == "attempting_door" || tag == "trying_door") {
    evidence = RiskEvidence::AttemptingDoor;
    return true;
  }
  if (tag == "raised_object" || tag == "weapon_like") {
    evidence = RiskEvidence::RaisedObject;
    return true;
  }
  if (tag == "aggressive" || tag == "aggression") {
    evidence = RiskEvidence::Aggressive;
    return true;
  }
  if (tag == "following_resident") {
    evidence = RiskEvidence::FollowingResident;
    return true;
  }
  if (tag == "carrying_box" || tag == "delivery") {
    evidence = RiskEvidence::CarryingBox;
    return true;
  }
  if (tag == "loitering") {
    evidence = RiskEvidence::Loitering;
    return true;
  }
  if (tag == "calm_delivery_reply" || tag == "delivery_reply") {
    evidence = RiskEvidence::CalmDeliveryReply;
    return true;
  }
  if (tag == "unintelligible_reply") {
    evidence = RiskEvidence::UnintelligibleReply;
    return true;
  }
  return false;
}

bool guard_risk::isRaisingEvidence(RiskEvidence evidence)
{
  switch (evidence) {
    case RiskEvidence::ConcealedFace:
    case RiskEvidence::AttemptingDoor:
    case RiskEvidence::RaisedObject:
    case RiskEvidence::Aggressive:
    case RiskEvidence::FollowingResident:
    case RiskEvidence::CarryingBox:
    case RiskEvidence::Loitering:
      return true;
    case RiskEvidence::CalmDeliveryReply:
    case RiskEvidence::UnintelligibleReply:
      return false;
  }
  return false;
}

int guard_risk::evidenceScore(RiskEvidence evidence)
{
  switch (evidence) {
    case RiskEvidence::AttemptingDoor:
    case RiskEvidence::RaisedObject:
      return 3;
    case RiskEvidence::ConcealedFace:
    case RiskEvidence::Aggressive:
    case RiskEvidence::FollowingResident:
      return 2;
    case RiskEvidence::CarryingBox:
    case RiskEvidence::Loitering:
      return 1;
    case RiskEvidence::CalmDeliveryReply:
    case RiskEvidence::UnintelligibleReply:
      return 0;
  }
  return 0;
}

std::string guard_risk::evidenceToString(RiskEvidence evidence)
{
  switch (evidence) {
    case RiskEvidence::ConcealedFace:
      return "concealed_face";
    case RiskEvidence::AttemptingDoor:
      return "attempting_door";
    case RiskEvidence::RaisedObject:
      return "raised_object";
    case RiskEvidence::Aggressive:
      return "aggressive";
    case RiskEvidence::FollowingResident:
      return "following_resident";
    case RiskEvidence::CarryingBox:
      return "carrying_box";
    case RiskEvidence::Loitering:
      return "loitering";
    case RiskEvidence::CalmDeliveryReply:
      return "calm_delivery_reply";
    case RiskEvidence::UnintelligibleReply:
      return "unintelligible_reply";
  }
  return "loitering";
}

GuardRiskResult guard_risk::mergeEvidence(const GuardRiskInput& input)
{
  GuardRiskResult result;
  result.danger = input.floor;
  result.appliedTags.reserve(input.tags.size());

  for (const auto& tag : input.tags) {
    RiskEvidence evidence;
    if (!evidenceFromTag(tag, evidence))
      continue;
    result.appliedTags.push_back(evidenceToString(evidence));
    result.evidenceScore += evidenceScore(evidence);
  }

  if (input.hardFloor || result.evidenceScore <= 0)
    return result;

  const bool sustainedThreat =
      input.threat == "medium" || input.threat == "high" ||
      input.threat == "critical";
  if (!sustainedThreat)
    return result;

  const int rank = guardDangerRank(result.danger);
  if (result.evidenceScore >= kRaiseThreshold && rank < guardDangerRank(GuardDanger::High))
    result.danger = GuardDanger::High;
  else if (result.evidenceScore >= kSoftThreshold &&
           rank < guardDangerRank(GuardDanger::Medium))
    result.danger = GuardDanger::Medium;
  return result;
}
