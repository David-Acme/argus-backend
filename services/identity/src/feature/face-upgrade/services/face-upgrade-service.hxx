#pragma once

#include <feature/face-upgrade/repositories/face-upgrade/face-upgrade-repository.hxx>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <optional>
#include <string>

struct FaceUpgradeReport
{
  int stale{0};
  int upgraded{0};
  int withoutPortrait{0};
  int withoutFace{0};
  int failed{0};
};

using PortraitReader =
    std::function<drogon::Task<std::optional<std::string>>(int64_t userId)>;

class FaceUpgradeService
{
public:
  explicit FaceUpgradeService(PortraitReader readPortrait);

  [[nodiscard]] drogon::Task<FaceUpgradeReport> run() const;

private:
  struct UpgradeOneInput
  {
    const FaceUpgradeCandidate& candidate;
    FaceUpgradeReport& report;
  };

  drogon::Task<void> upgradeOne(UpgradeOneInput input) const;

  PortraitReader readPortrait_;
  FaceUpgradeRepository repository_;
  FaceEmbeddingRepository faceEmbeddingRepository_;
};
