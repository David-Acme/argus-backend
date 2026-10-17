#include "clip-service.hxx"

#include "opaque-token.hxx"

#include <algorithm>
#include <auth/module-gate.hxx>
#include <auth/role-access.hxx>
#include <camera/camera-errors.hxx>
#include <ctime>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <errors/response-exception.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>
#include <utility>

namespace
{
constexpr int64_t kCapabilityLifetimeSeconds = 60;
constexpr std::string_view kClipListProbe = "/camera/0/clips";
constexpr std::string_view kClipDeleteProbe = "/camera/0/clips/0";
constexpr std::string_view kClipContentProbe = "/clip/0/content";

bool allowsRole(UserRole role, std::string_view path, drogon::HttpMethod method)
{
  return role_access::hasHttpAccess(
      {.role = role,
       .path = path,
       .method = method,
       .roleActive = moduleGate().roleActive(role)});
}
}

std::string ClipService::hashToken(const std::string& token)
{
  auto hash = recording_token::sha256Hex(token);
  if (!hash)
    throw ResponseException(CameraErrors::ClipPreviewPreparationFailed);
  return std::move(*hash);
}

std::string ClipService::newToken()
{
  auto token = recording_token::mint();
  if (!token)
    throw ResponseException(CameraErrors::ClipPreviewPreparationFailed);
  return std::move(*token);
}

drogon::Task<ResponseClipsDto>
ClipService::list(const ClipListInput& input) const
{
  if (!allowsRole(input.requesterRole, kClipListProbe, drogon::Get) ||
      input.cameraId <= 0)
    throw ResponseException(403, CameraErrors::Forbidden);

  const auto camera = co_await cameraRepository_.findById(input.cameraId);
  if (!camera)
    throw ResponseException(CameraErrors::CameraNotFound);

  ResponseClipsDto response;
  response.cameraId = input.cameraId;
  response.clips = co_await clipRepository_.findByCamera(
      {.cameraId = input.cameraId,
       .beforeCreatedAt = input.beforeCreatedAt,
       .beforeId = input.beforeId,
       .limit = std::clamp<int64_t>(input.limit, 1, kMaxPageSize)});
  co_return response;
}

drogon::Task<ResponseClipPreviewCapabilityDto>
ClipService::createPreview(const ClipPreviewCreateInput& input) const
{
  if (!allowsRole(input.requesterRole, kClipListProbe, drogon::Get))
    throw ResponseException(403, CameraErrors::Forbidden);
  if (input.clipId <= 0)
    throw ResponseException(CameraErrors::InvalidClipId);

  const auto clip = co_await clipRepository_.findById(input.clipId);
  if (!clip || clip->cameraId != input.cameraId)
    throw ResponseException(CameraErrors::ClipNotFound);

  const auto token = newToken();
  const int64_t now = std::time(nullptr);
  const int64_t expiresAt = now + kCapabilityLifetimeSeconds;
  co_await capabilityRepository_.purgeSpent(now);
  co_await capabilityRepository_.create({.tokenHash = hashToken(token),
                                         .clipId = clip->id,
                                         .requesterUserId = input.requesterUserId,
                                         .expiresAt = expiresAt});
  co_return {.token = token, .expiresAt = expiresAt};
}

drogon::Task<ResponseClipContentDto>
ClipService::consumePreview(const ClipPreviewConsumeInput& input) const
{
  if (!allowsRole(input.requesterRole, kClipContentProbe, drogon::Get))
    throw ResponseException(403, CameraErrors::Forbidden);
  if (input.token.empty())
    throw ResponseException(CameraErrors::ClipPreviewUnavailable);

  std::optional<ClipPreviewCapabilitySchema> capability;
  std::optional<CameraClipSchema> clip;
  auto transaction = co_await db_transaction::begin(DbService::cameraClient());
  try {
    capability = co_await capabilityRepository_.findByTokenHash(
        hashToken(input.token), transaction.get());
    const int64_t now = std::time(nullptr);
    if (!capability || capability->requesterUserId != input.requesterUserId)
      throw ResponseException(CameraErrors::ClipPreviewUnavailable);

    const bool consumed = co_await capabilityRepository_.tryConsume(
        {.id = capability->id,
         .requesterUserId = input.requesterUserId,
         .now = now,
         .client = transaction.get()});
    if (!consumed)
      throw ResponseException(CameraErrors::ClipPreviewUnavailable);

    clip = co_await clipRepository_.findById(capability->clipId,
                                             transaction.get());
    if (!clip)
      throw ResponseException(CameraErrors::ClipNotFound);

    Json::Value event;
    event["event"] = "clip_view";
    event["cameraId"] = clip->cameraId;
    event["clipId"] = clip->id;
    event["durationMs"] = clip->durationMs;
    if (const auto* sink = camera_change::getSink()) {
      co_await sink->publishAction(
          {.event = {.userId = input.requesterUserId,
                     .recordId = clip->id,
                     .tableName = TableName::Camera,
                     .action = UserAction::Read,
                     .oldData = Json::Value(),
                     .newData = event,
                     .ipAddress = ""},
           .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  std::string body;
  try {
    body = co_await storage_.get(clip->objectKey);
  }
  catch (const std::exception&) {
    throw ResponseException(CameraErrors::ClipUnavailable);
  }
  co_return {.mimeType = clip->contentType,
              .base64 = drogon::utils::base64Encode(body),
              .durationMs = clip->durationMs};
}

drogon::Task<void> ClipService::remove(const ClipDeleteInput& input) const
{
  if (!allowsRole(input.requesterRole, kClipDeleteProbe, drogon::Delete))
    throw ResponseException(403, CameraErrors::Forbidden);
  if (input.clipId <= 0)
    throw ResponseException(CameraErrors::InvalidClipId);

  const auto clip = co_await clipRepository_.findById(input.clipId);
  if (!clip || clip->cameraId != input.cameraId)
    throw ResponseException(CameraErrors::ClipNotFound);

  const int64_t now = std::time(nullptr);
  if (!clip->objectKey.empty()) {
    try {
      co_await storage_.remove(clip->objectKey);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera recording: clip " << clip->id
               << " object removal failed: " << error.what();
    }
  }

  auto transaction = co_await db_transaction::begin(DbService::cameraClient());
  try {
    if (!co_await clipRepository_.markDeleted(
            {.id = clip->id, .deletedAt = now, .client = transaction.get()}))
      throw ResponseException(CameraErrors::ClipNotFound);

    Json::Value event;
    event["event"] = "clip_delete";
    event["cameraId"] = clip->cameraId;
    event["clipId"] = clip->id;
    event["durationMs"] = clip->durationMs;
    if (const auto* sink = camera_change::getSink()) {
      co_await sink->publishAction(
          {.event = {.userId = input.requesterUserId,
                     .recordId = clip->id,
                     .tableName = TableName::Camera,
                     .action = UserAction::Delete,
                     .oldData = Json::Value(),
                     .newData = event,
                     .ipAddress = ""},
           .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  if (const auto* sink = camera_change::getSink()) {
    co_await sink->publishResource(
        {.scope = ResourceScope::Clips, .ids = {clip->cameraId}, .user = 0,
         .client = nullptr});
  }
  co_return;
}
