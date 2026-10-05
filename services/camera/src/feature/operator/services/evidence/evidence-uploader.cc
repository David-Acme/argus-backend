#include "evidence-uploader.hxx"

#include <drogon/drogon.h>
#include <shared/services/stream/snapshot-store.hxx>
#include <shared/utils/in-flight/in-flight.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <array>
#include <ctime>
#include <exception>
#include <string>
#include <utility>

namespace
{
constexpr int64_t kDaySeconds = 24LL * 3600;
constexpr int kSweepBatch = 200;
constexpr int kMaxSweepBatches = 50;

constexpr const char* kInsertEvidence =
    "INSERT INTO camera_evidence (camera_id, object_key, content_type, "
    "created_at, expires_at) VALUES (?, ?, ?, ?, ?)";

constexpr const char* kCameraRetention =
    "SELECT retention_days, config FROM camera WHERE id = ?";

constexpr const char* kExpiredEvidence =
    "SELECT id, object_key FROM camera_evidence "
    "WHERE deleted_at = 0 AND id > ? AND "
    "((expires_at > 0 AND expires_at <= ?) OR created_at <= ?) "
    "ORDER BY id ASC LIMIT 200";

constexpr const char* kMarkEvidenceDeleted =
    "UPDATE camera_evidence SET deleted_at = ? WHERE id = ?";

constexpr const char* kCountExpired =
    "SELECT COUNT(*) AS total FROM camera_evidence WHERE deleted_at = 0 AND "
    "((expires_at > 0 AND expires_at <= ?) OR created_at <= ?)";
}

EvidenceUploader& EvidenceUploader::instance()
{
  static EvidenceUploader uploader;
  return uploader;
}

int64_t EvidenceUploader::retentionSecondsOf(const EvidenceRetentionInput& input)
{
  const int64_t cap = input.incident ? kMaxIncidentRetentionDays : kMaxRetentionDays;
  const int64_t days = std::clamp<int64_t>(input.retentionDays.value_or(kDefaultRetentionDays), 0, cap);
  return days * kDaySeconds;
}

void EvidenceUploader::uploadDetection(int64_t cameraId, int64_t atMs)
{
  if (stopping_.load(std::memory_order_acquire) || !storage_.isConfigured())
    return;
  const auto frame = SnapshotStore::instance().frame(cameraId);
  const auto crop = SnapshotStore::instance().latestPersonCrop(cameraId);
  std::string frameJpeg = frame ? frame->jpeg : std::string{};
  std::string cropJpeg = crop ? crop->jpeg : std::string{};
  if (frameJpeg.empty() && cropJpeg.empty())
    return;

  inFlight_.fetch_add(1, std::memory_order_acq_rel);
  drogon::app().getIOLoop(0)->runInLoop(
      [this, cameraId, atMs, frameJpeg = std::move(frameJpeg),
       cropJpeg = std::move(cropJpeg)]() mutable {
        drogon::async_run(
            [this, cameraId, atMs, frameJpeg = std::move(frameJpeg),
             cropJpeg = std::move(cropJpeg)]() mutable -> drogon::Task<void> {
              try {
                const auto client = DbService::client();
                const auto camera = co_await client->execSqlCoro(kCameraRetention, cameraId);
                const std::optional<int64_t> days =
                    camera.empty() || camera.front()["retention_days"].isNull()
                        ? std::nullopt
                        : std::optional(camera.front()["retention_days"].as<int64_t>());
                const bool incident =
                    !camera.empty() &&
                    camera_stream_paths::retentionIncidentOf(camera.front()["config"].as<std::string>());
                const int64_t keepSeconds =
                    retentionSecondsOf({.retentionDays = days, .incident = incident});
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                const std::string prefix = "cameras/" + std::to_string(cameraId) +
                                           "/" + std::to_string(atMs);
                const std::array<std::pair<std::string, const std::string*>, 2> parts{
                    std::pair<std::string, const std::string*>{prefix + "_frame.jpg", &frameJpeg},
                    std::pair<std::string, const std::string*>{prefix + "_person.jpg", &cropJpeg}};
                if (keepSeconds > 0 && !stopping_.load(std::memory_order_acquire)) {
                  for (const auto& [key, jpeg] : parts) {
                    if (jpeg->empty())
                      continue;
                    co_await storage_.put({.objectKey = key, .body = *jpeg, .contentType = "image/jpeg"});
                    co_await client->execSqlCoro(kInsertEvidence, cameraId, key, "image/jpeg", now,
                                                 now + keepSeconds);
                  }
                }
              }
              catch (const std::exception& error) {
                LOG_WARN << "Camera evidence upload failed: " << error.what();
              }
              catch (...) {
                LOG_WARN << "Camera evidence upload failed with unknown error";
              }
              inFlight_.fetch_sub(1, std::memory_order_acq_rel);
            });
      });
}

void EvidenceUploader::scheduleRetentionSweep()
{
  const auto sweepOnce = [this]() -> drogon::Task<void> {
    if (stopping_.load(std::memory_order_acquire))
      co_return;
    const in_flight::Guard guard(inFlight_);
    try {
      const auto report = co_await runRetentionSweep(static_cast<int64_t>(std::time(nullptr)));
      if (report.removed > 0 || report.failed > 0)
        LOG_INFO << "Camera retention: removed " << report.removed
                 << " expired evidence object(s), " << report.failed
                 << " failed and retried next sweep";
      if (report.stranded > 0)
        LOG_WARN << "Camera retention: " << report.stranded
                 << " expired evidence object(s) wait for object storage to be configured";
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera evidence retention sweep failed: " << error.what();
    }
    catch (...) {
      LOG_WARN << "Camera evidence retention sweep failed with unknown error";
    }
    co_return;
  };
  drogon::app().getLoop()->runAfter(45.0, [sweepOnce]() {
    drogon::async_run(sweepOnce);
  });
  drogon::app().getLoop()->runEvery(6.0 * 3600.0, [sweepOnce]() {
    drogon::async_run(sweepOnce);
  });
}

drogon::Task<EvidenceSweepReport> EvidenceUploader::runRetentionSweep(int64_t now)
{
  EvidenceSweepReport report;
  const int64_t hardCap = now - kMaxIncidentRetentionDays * kDaySeconds;
  const auto client = DbService::client();
  if (!storage_.isConfigured()) {
    const auto stranded = co_await client->execSqlCoro(kCountExpired, now, hardCap);
    report.stranded = stranded.empty() ? 0 : stranded.front()["total"].as<int64_t>();
    co_return report;
  }
  int64_t cursor = 0;
  for (int batch = 0; batch < kMaxSweepBatches && !stopping_.load(std::memory_order_acquire);
       ++batch) {
    const auto expired = co_await client->execSqlCoro(kExpiredEvidence, cursor, now, hardCap);
    if (expired.empty())
      break;
    for (const auto& row : expired) {
      const int64_t id = row["id"].as<int64_t>();
      cursor = std::max(cursor, id);
      const std::string key = row["object_key"].as<std::string>();
      try {
        co_await storage_.remove(key);
      }
      catch (const std::exception& error) {
        ++report.failed;
        LOG_WARN << "Camera evidence removal failed for evidence " << id << ": " << error.what();
        continue;
      }
      co_await client->execSqlCoro(kMarkEvidenceDeleted, now, id);
      ++report.removed;
    }
    if (expired.size() < static_cast<size_t>(kSweepBatch))
      break;
  }
  co_return report;
}

void EvidenceUploader::requestStop()
{
  stopping_.store(true, std::memory_order_release);
}

bool EvidenceUploader::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}
