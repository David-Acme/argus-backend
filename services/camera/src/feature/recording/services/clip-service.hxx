#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/recording/dtos/response-clip-content-dto.hxx>
#include <feature/recording/dtos/response-clip-preview-capability-dto.hxx>
#include <feature/recording/dtos/response-clips-dto.hxx>
#include <feature/recording/repositories/camera-clip/camera-clip-repository.hxx>
#include <feature/recording/repositories/clip-preview-capability/clip-preview-capability-repository.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <storage/s3-storage-service.hxx>
#include <string>

struct ClipListInput
{
  int64_t cameraId{0};
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
  int64_t beforeCreatedAt{0};
  int64_t beforeId{0};
  int64_t limit{50};
};

struct ClipPreviewCreateInput
{
  int64_t cameraId{0};
  int64_t clipId{0};
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
};

struct ClipPreviewConsumeInput
{
  std::string token;
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
};

struct ClipDeleteInput
{
  int64_t cameraId{0};
  int64_t clipId{0};
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
};

class ClipService
{
public:
  drogon::Task<ResponseClipsDto> list(const ClipListInput& input) const;
  drogon::Task<ResponseClipPreviewCapabilityDto>
  createPreview(const ClipPreviewCreateInput& input) const;
  drogon::Task<ResponseClipContentDto>
  consumePreview(const ClipPreviewConsumeInput& input) const;
  drogon::Task<void> remove(const ClipDeleteInput& input) const;

  static constexpr int64_t kMaxPageSize = 200;

private:
  static std::string hashToken(const std::string& token);
  static std::string newToken();

  CameraRepository cameraRepository_;
  CameraClipRepository clipRepository_;
  ClipPreviewCapabilityRepository capabilityRepository_;
  S3StorageService storage_;
};
