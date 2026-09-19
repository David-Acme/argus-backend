#pragma once

#include <shared/enums.hxx>

#include <string>
#include <vector>

// Closed observable vocabulary the model may report; unknown tags are dropped.
enum class RiskEvidence
{
  ConcealedFace = 0,
  AttemptingDoor,
  RaisedObject,
  Aggressive,
  FollowingResident,
  CarryingBox,
  Loitering,
  CalmDeliveryReply,
  UnintelligibleReply
};

namespace guard_risk
{

// Parses an observable tag; nullopt for anything outside the vocabulary.
bool evidenceFromTag(const std::string& tag, RiskEvidence& evidence);

bool isRaisingEvidence(RiskEvidence evidence);

int evidenceScore(RiskEvidence evidence);

std::string evidenceToString(RiskEvidence evidence);

} // namespace guard_risk

struct GuardRiskInput
{
  GuardDanger floor{GuardDanger::None};
  std::string threat;
  std::vector<std::string> tags;
  bool hardFloor{false};
};

struct GuardRiskResult
{
  GuardDanger danger{GuardDanger::None};
  int evidenceScore{0};
  std::vector<std::string> appliedTags;
};

namespace guard_risk
{

// Bounded semantic evidence may raise a soft case, never lower a hard floor.
GuardRiskResult mergeEvidence(const GuardRiskInput& input);

} // namespace guard_risk
