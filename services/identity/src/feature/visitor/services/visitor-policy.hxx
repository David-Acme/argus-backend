#pragma once

#include <shared/services/face/face-quality.hxx>

#include <cstdint>
#include <optional>
#include <span>

enum class PersonPool : std::uint8_t
{
  Household,
  Named,
  Unnamed
};

struct PoolNeighbour
{
  int64_t personId{0};
  float score{0.0F};
  PersonPool pool{PersonPool::Unnamed};
};

struct VisitorThresholds
{
  float householdMatch{0.0F};
  float householdGuard{0.0F};
  float visitorMatch{0.0F};
  float visitorNewCeiling{0.0F};
  float visitorMargin{0.0F};
  float sampleConsistency{0.0F};
  float duplicateSimilarity{0.0F};
  FaceQualityGate matchGate;
  FaceQualityGate learnGate;
  int maxSamples{0};
};

enum class SightingOutcome : std::uint8_t
{
  Disabled,
  LowQuality,
  Household,
  NearHousehold,
  Visitor,
  Ambiguous,
  Uncertain,
  NewVisitor
};

struct SightingDecision
{
  SightingOutcome outcome{SightingOutcome::Disabled};
  int64_t personId{0};
  float score{0.0F};
  bool learn{false};
};

struct DecideSightingInput
{
  std::span<const PoolNeighbour> neighbours;
  const FaceQuality& quality;
  bool recognitionEnabled{false};
  const VisitorThresholds& thresholds;
};

enum class SampleAdmissionKind : std::uint8_t
{
  Add,
  Replace,
  Skip,
  Outlier
};

struct SampleAdmission
{
  SampleAdmissionKind kind{SampleAdmissionKind::Skip};
  std::size_t replaceIndex{0};
};

struct AdmitSampleInput
{
  std::span<const float> similarities;
  std::span<const float> existingQualities;
  float quality{0.0F};
  const VisitorThresholds& thresholds;
};

namespace visitor_policy
{
[[nodiscard]] VisitorThresholds defaults();
[[nodiscard]] SightingDecision decide(const DecideSightingInput& input);
[[nodiscard]] SampleAdmission admit(const AdmitSampleInput& input);
[[nodiscard]] float median(std::span<const float> values);
}
