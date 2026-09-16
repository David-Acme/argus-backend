#include "evidence-uploader.hxx"

#include <drogon/drogon.h>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/snapshot-store.hxx>

#include <ctime>
#include <exception>
#include <string>
#include <utility>

namespace
{
constexpr int64_t kEvidenceRetentionS = 7LL * 24 * 3600;

constexpr const char* kInsertEvidence =
    "INSERT INTO camera_evidence (camera_id, object_key, content_type, "
    "created_at, expires_at) VALUES (?, ?, ?, ?, ?)";

constexpr const char* kExpiredEvidence =
    "SELECT id, object_key FROM camera_evidence "
    "WHERE deleted_at = 0 AND expires_at > 0 AND expires_at <= ? LIMIT 200";

constexpr const char* kMarkEvidenceDeleted =
    "UPDATE camera_evidence SET deleted_at = ? WHERE id = ?";
} // namespace

EvidenceUploader& EvidenceUploader::instance()
{
  static EvidenceUploader uploader;
  return uploader;
}

void EvidenceUploader::uploadDetection(int64_t cameraId, int64_t atMs)
{
  const auto frame = SnapshotStore::instance().frame(cameraId);
  const auto crop = SnapshotStore::instance().latestPersonCrop(cameraId);
  const std::string frameJpeg = frame ? frame->jpeg : std::string{};
  const std::string cropJpeg = crop ? crop->jpeg : std::string{};
  if (frameJpeg.empty() && cropJpeg.empty())
    return;

  if (!storage_.isConfigured())
    return;

  drogon::app().getIOLoop(0)->runInLoop(
      [this, cameraId, atMs, frameJpeg, cropJpeg]() mutable {
        drogon::async_run(
            [this, cameraId, atMs, frameJpeg = std::move(frameJpeg),
             cropJpeg = std::move(cropJpeg)]() mutable -> drogon::Task<void> {
              const std::string prefix = "cameras/" + std::to_string(cameraId) +
                                         "/" + std::to_string(atMs);
              const int64_t now = static_cast<int64_t>(std::time(nullptr));
              const int64_t expiresAt = now + kEvidenceRetentionS;
              try {
                if (!frameJpeg.empty()) {
                  const std::string key = prefix + "_frame.jpg";
                  co_await storage_.put({.objectKey = key,
                                         .body = frameJpeg,
                                         .contentType = "image/jpeg"});
                  co_await DbService::client()->execSqlCoro(
                      kInsertEvidence, cameraId, key, "image/jpeg", now,
                      expiresAt);
                }
                if (!cropJpeg.empty()) {
                  const std::string key = prefix + "_person.jpg";
                  co_await storage_.put({.objectKey = key,
                                         .body = cropJpeg,
                                         .contentType = "image/jpeg"});
                  co_await DbService::client()->execSqlCoro(
                      kInsertEvidence, cameraId, key, "image/jpeg", now,
                      expiresAt);
                }
              }
              catch (const std::exception& error) {
                LOG_WARN << "Camera evidence upload failed: " << error.what();
              }
              catch (...) {
                LOG_WARN << "Camera evidence upload failed with unknown error";
              }
            });
      });
}

void EvidenceUploader::scheduleRetentionSweep()
{
  const auto sweepOnce = [this]() -> drogon::Task<void> {
    try {
      co_await runRetentionSweep();
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera evidence retention sweep failed: " << error.what();
    }
    catch (...) {
      LOG_WARN << "Camera evidence retention sweep failed with unknown error";
    }
    co_return;
  };
  drogon::app().getLoop()->runAfter(45.0, [this, sweepOnce]() {
    drogon::async_run(sweepOnce);
  });
  drogon::app().getLoop()->runEvery(24.0 * 3600.0, [this, sweepOnce]() {
    drogon::async_run(sweepOnce);
  });
}

drogon::Task<void> EvidenceUploader::runRetentionSweep()
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!storage_.isConfigured()) {
    LOG_INFO << "Camera retention: object storage not configured; skipped";
    co_return;
  }
  int64_t removed = 0;
  for (int batch = 0; batch < 50; ++batch) {
    const auto expired =
        co_await DbService::client()->execSqlCoro(kExpiredEvidence, now);
    if (expired.empty())
      break;
    for (const auto& row : expired) {
      const std::string key = row["object_key"].as<std::string>();
      try {
        co_await storage_.remove(key);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera evidence removal failed: " << error.what();
        continue;
      }
      catch (...) {
        LOG_WARN << "Camera evidence removal failed with unknown error";
        continue;
      }
      co_await DbService::client()->execSqlCoro(
          kMarkEvidenceDeleted, now, row["id"].as<int64_t>());
      ++removed;
    }
    if (expired.size() < 200)
      break;
  }
  if (removed > 0)
    LOG_INFO << "Camera retention: removed " << removed
             << " expired evidence object(s)";
  co_return;
}
