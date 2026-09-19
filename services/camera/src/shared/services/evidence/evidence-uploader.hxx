#pragma once

#include <cstdint>
#include <shared/services/storage/s3-storage-service.hxx>

// Uploads detection evidence (full frame and person crop) to the configured
// private object store, records a retention manifest and sweeps expired rows.
class EvidenceUploader
{
public:
  static EvidenceUploader& instance();

  void uploadDetection(int64_t cameraId, int64_t atMs);

  void scheduleRetentionSweep();

private:
  EvidenceUploader() = default;

  drogon::Task<void> runRetentionSweep();

  S3StorageService storage_;
};
