#pragma once

#include <cstdint>
#include <storage/s3-storage-service.hxx>

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
