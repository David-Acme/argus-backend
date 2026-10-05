#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <storage/s3-storage-service.hxx>

struct EvidenceRetentionInput
{
  std::optional<int64_t> retentionDays;
  bool incident{false};
};

struct EvidenceSweepReport
{
  int64_t removed{0};
  int64_t failed{0};
  int64_t stranded{0};
};

class EvidenceUploader
{
public:
  static constexpr int64_t kDefaultRetentionDays = 7;
  static constexpr int64_t kMaxRetentionDays = 60;
  static constexpr int64_t kMaxIncidentRetentionDays = 120;

  static EvidenceUploader& instance();

  [[nodiscard]] static int64_t retentionSecondsOf(const EvidenceRetentionInput& input);

  void uploadDetection(int64_t cameraId, int64_t atMs);

  void scheduleRetentionSweep();
  drogon::Task<EvidenceSweepReport> runRetentionSweep(int64_t now);

  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  EvidenceUploader() = default;

  S3StorageService storage_;
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> inFlight_{0};
};
