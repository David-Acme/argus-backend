#include "face-upgrade-service.hxx"

#include <drogon/drogon.h>
#include <runtime/blocking-task.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/vocabulary/face-model.hxx>

#include <exception>
#include <utility>

FaceUpgradeService::FaceUpgradeService(PortraitReader readPortrait)
    : readPortrait_(std::move(readPortrait))
{
}

drogon::Task<FaceUpgradeReport> FaceUpgradeService::run() const
{
  FaceUpgradeReport report;
  if (!FaceService::instance().isLoaded())
    co_return report;
  const auto candidates =
      co_await repository_.findStaleUserPersons(kFaceModelId);
  report.stale = static_cast<int>(candidates.size());
  for (const auto& candidate : candidates)
    co_await upgradeOne({.candidate = candidate, .report = report});
  if (report.stale > 0)
    LOG_INFO << "Face model upgrade: " << report.upgraded << " of "
             << report.stale << " account(s) re-embedded from their portrait";
  if (report.upgraded < report.stale)
    LOG_WARN << "Face model upgrade: " << report.withoutPortrait
             << " account(s) without a portrait, " << report.withoutFace
             << " without a usable face, " << report.failed
             << " failed; they sign in by QR from a paired device until "
                "their face is registered again";
  co_return report;
}

drogon::Task<void> FaceUpgradeService::upgradeOne(UpgradeOneInput input) const
{
  const FaceUpgradeCandidate& candidate = input.candidate;
  FaceUpgradeReport& report = input.report;
  try {
    auto portrait = co_await readPortrait_(candidate.userId);
    if (!portrait || portrait->empty()) {
      ++report.withoutPortrait;
      co_return;
    }
    const auto face = co_await FaceService::instance().analyzeImageAsync(
        {.imageBytes = std::move(*portrait), .encodeFace = false});
    if (!face) {
      ++report.withoutFace;
      co_return;
    }
    const std::string embedding(
        reinterpret_cast<const char*>(face->embedding.data()),
        face->embedding.size() * sizeof(float));
    const auto row = co_await faceEmbeddingRepository_.create(
        {.personId = candidate.personId,
         .embedding = embedding,
         .angleLabel = "frontal",
         .quality = face->quality.detectorScore,
         .model = std::string(kFaceModelId),
         .client = nullptr});
    const bool indexed = co_await BlockingTask<bool>(
        [vector = face->embedding, personId = candidate.personId,
         rowId = row.id] {
          return FaceService::instance().faceDb().insert(
              {.embedding = vector.data(),
               .personId = personId,
               .faceEmbeddingId = rowId});
        });
    if (indexed)
      ++report.upgraded;
    else
      ++report.failed;
  }
  catch (const std::exception& error) {
    ++report.failed;
    LOG_WARN << "Face model upgrade failed for person " << candidate.personId
             << ": " << error.what();
  }
}
