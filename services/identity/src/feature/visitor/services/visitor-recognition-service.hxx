#pragma once

#include <shared/services/face-crop/face-crop-store.hxx>
#include <feature/visitor/repositories/visitor/visitor-repository.hxx>
#include <feature/visitor/services/visitor-policy.hxx>
#include <identity/person-category.hxx>
#include <runtime/blocking-pool.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/privacy/privacy-gate.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <vector>

struct SightingInput
{
  std::string image;
  int64_t cameraId{0};
  int64_t observedAt{0};
};

struct SightingResult
{
  bool faceFound{false};
  FaceQualityVerdict quality{FaceQualityVerdict::Accepted};
  SightingDecision decision;
  bool created{false};
  bool newVisit{false};
  int64_t visits{0};
  std::optional<int64_t> visitorNumber;
  std::optional<int64_t> userId;
  PersonCategory category{PersonCategory::None};
  bool named{false};
  bool known{false};
};

class VisitorRecognitionService
{
public:
  VisitorRecognitionService();

  drogon::Task<SightingResult> observe(SightingInput input);

  static constexpr int64_t kVisitGapSeconds = 600;
  static constexpr int kNeighbours = 24;

private:
  struct Recorded
  {
    SightingResult result;
    int64_t sampleId{0};
    std::vector<std::string> obsoleteCrops;
    std::vector<int64_t> householdEchoes;
  };

  struct RecordInput
  {
    const FaceService::FaceAnalysis& analysis;
    int64_t cameraId{0};
    int64_t observedAt{0};
    bool enabled{false};
  };

  [[nodiscard]] Recorded record(const RecordInput& input) const;
  drogon::Task<void> forgetEchoes(std::vector<int64_t> personIds);

  VisitorThresholds thresholds_;
  BlockingStrand strand_;
  PrivacyGate privacyGate_;
  FaceCropStore cropStore_;
  VisitorRepository visitorRepository_;
};
