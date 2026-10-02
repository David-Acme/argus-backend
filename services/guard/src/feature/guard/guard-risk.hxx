#pragma once

#include <feature/guard/vocabulary/guard-danger.hxx>

#include <string>
#include <vector>

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
  UnintelligibleReply,
  Weapon
};

namespace guard_risk
{

bool evidenceFromTag(const std::string& tag, RiskEvidence& evidence);

bool isRaisingEvidence(RiskEvidence evidence);

int evidenceScore(RiskEvidence evidence);

std::string evidenceToString(RiskEvidence evidence);

}

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
  bool weapon{false};
  std::vector<std::string> appliedTags;
};

namespace guard_risk
{

GuardRiskResult mergeEvidence(const GuardRiskInput& input);

}
