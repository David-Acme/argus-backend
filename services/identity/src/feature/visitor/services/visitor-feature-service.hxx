#pragma once

#include <auth/user-role.hxx>
#include <feature/visitor/dtos/merge-visitors-dto.hxx>
#include <feature/visitor/dtos/response-visitor-crop-dto.hxx>
#include <feature/visitor/dtos/response-visitor-detail-dto.hxx>
#include <feature/visitor/dtos/response-visitor-dto.hxx>
#include <feature/visitor/dtos/response-visitor-settings-dto.hxx>
#include <feature/visitor/dtos/split-visitor-dto.hxx>
#include <feature/visitor/dtos/update-visitor-dto.hxx>
#include <feature/visitor/dtos/update-visitor-settings-dto.hxx>
#include <feature/visitor/repositories/crop-capability/crop-capability-repository.hxx>
#include <feature/visitor/repositories/visitor/visitor-repository.hxx>
#include <shared/repositories/visitor-setting/visitor-setting-repository.hxx>
#include <shared/services/face-crop/face-crop-store.hxx>
#include <shared/services/privacy/privacy-gate.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>

struct VisitorRequester
{
  int64_t userId{0};
  UserRole role{UserRole::Guest};
};

struct VisitorListRequest
{
  VisitorRequester requester;
  bool namedOnly{false};
};

struct VisitorRequest
{
  VisitorRequester requester;
  int64_t personId{0};
};

struct VisitorUpdateRequest
{
  VisitorRequester requester;
  int64_t personId{0};
  UpdateVisitorDto body;
};

struct VisitorMergeRequest
{
  VisitorRequester requester;
  int64_t personId{0};
  MergeVisitorsDto body;
};

struct VisitorSplitRequest
{
  VisitorRequester requester;
  int64_t personId{0};
  SplitVisitorDto body;
};

struct VisitorSampleRequest
{
  VisitorRequester requester;
  int64_t personId{0};
  int64_t sampleId{0};
};

struct VisitorCropConsumeRequest
{
  VisitorRequester requester;
  std::string token;
};

struct VisitorSettingsUpdateRequest
{
  VisitorRequester requester;
  UpdateVisitorSettingsDto body;
};

class VisitorFeatureService
{
public:
  drogon::Task<ResponseVisitorListDto> list(const VisitorListRequest& request) const;
  drogon::Task<ResponseVisitorDetailDto> detail(const VisitorRequest& request) const;
  drogon::Task<ResponseVisitorDetailDto>
  update(const VisitorUpdateRequest& request) const;
  drogon::Task<ResponseVisitorDetailDto>
  merge(const VisitorMergeRequest& request) const;
  drogon::Task<ResponseVisitorDetailDto>
  split(const VisitorSplitRequest& request) const;
  drogon::Task<void> remove(const VisitorRequest& request) const;
  drogon::Task<void> removeSample(const VisitorSampleRequest& request) const;
  drogon::Task<ResponseVisitorCropCapabilityDto>
  mintCrop(const VisitorSampleRequest& request) const;
  drogon::Task<ResponseVisitorCropImageDto>
  consumeCrop(const VisitorCropConsumeRequest& request) const;
  drogon::Task<ResponseVisitorSettingsDto> settings() const;
  drogon::Task<ResponseVisitorSettingsDto>
  updateSettings(const VisitorSettingsUpdateRequest& request) const;

  static constexpr int64_t kCropLifetimeSeconds = 60;
  static constexpr std::size_t kMaxSamplesAfterMerge = 8;

private:
  struct JournalInput
  {
    int64_t actorId{0};
    int64_t personId{0};
    std::string event;
    Json::Value details;
  };

  drogon::Task<VisitorRow> requireVisible(const VisitorRequest& request) const;
  drogon::Task<ResponseVisitorDetailDto> detailOf(int64_t personId) const;
  drogon::Task<void> journal(const JournalInput& input) const;
  drogon::Task<void> trimSamples(int64_t personId) const;
  static void requireOwner(const VisitorRequester& requester);

  VisitorRepository repository_;
  CropCapabilityRepository cropCapabilityRepository_;
  VisitorSettingRepository settingRepository_;
  PrivacyGate privacyGate_;
  FaceCropStore cropStore_;
};
