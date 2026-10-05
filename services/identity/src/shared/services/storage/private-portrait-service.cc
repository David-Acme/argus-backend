#include "private-portrait-service.hxx"

#include <storage/s3-storage-service.hxx>

#include <drogon/drogon.h>
#include <optional>
#include <stdexcept>

drogon::Task<void>
PrivatePortraitService::store(int64_t userId, const std::string& portraitJpeg) const
{
  if (portraitJpeg.empty())
    co_return;
  S3StorageService storage;
  if (!storage.isConfigured()) {
    LOG_WARN << "Portrait storage is not configured; skipping portrait for user "
             << userId;
    co_return;
  }

  std::optional<S3StoredObject> object;
  std::string failure;
  try {
    object = co_await storage.putPortrait(userId, portraitJpeg);
    const auto previous = co_await portraitRepository_.findByUserId(userId);
    const auto file = co_await fileRepository_.create({
        .objectKey = object->objectKey,
        .sha256 = object->sha256,
        .mimeType = std::string(kPortraitMimeType),
        .byteSize = object->byteSize,
        .category = StoredFileCategory::Portrait,
        .createdBy = userId,
    });
    co_await portraitRepository_.upsertCurrent({.userId = userId, .fileId = file.id});
    if (previous && previous->fileId != file.id) {
      if (const auto old = co_await fileRepository_.findById(previous->fileId)) {
        co_await fileRepository_.remove(old->id);
        const PendingObjectEnqueueInput pending{.objectKeys = {old->objectKey},
                                                .client = nullptr};
        co_await pendingRepository_.enqueue(pending);
      }
    }
    co_return;
  }
  catch (const std::exception& error) {
    failure = error.what();
  }
  catch (...) {
    failure = "unknown error";
  }

  if (object) {
    try {
      co_await storage.remove(object->objectKey);
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Failed to remove orphaned private portrait for user "
                << userId << ": " << error.what();
    }
  }
  LOG_ERROR << "Failed to persist private portrait for user " << userId << ": "
            << failure;
  co_return;
}

drogon::Task<std::optional<PrivatePortrait>>
PrivatePortraitService::read(int64_t userId) const
{
  const auto portrait = co_await portraitRepository_.findByUserId(userId);
  if (!portrait)
    co_return std::nullopt;

  const auto file = co_await fileRepository_.findById(portrait->fileId);
  if (!file || file->category != StoredFileCategory::Portrait)
    co_return std::nullopt;

  S3StorageService storage;
  if (!storage.isConfigured())
    throw std::runtime_error("Private portrait storage is not configured");

  co_return PrivatePortrait{
      .mimeType = file->mimeType,
      .bytes = co_await storage.get(file->objectKey),
  };
}

drogon::Task<bool> PrivatePortraitService::has(int64_t userId) const
{
  const auto portrait = co_await portraitRepository_.findByUserId(userId);
  if (!portrait)
    co_return false;

  const auto file = co_await fileRepository_.findById(portrait->fileId);
  co_return file && file->category == StoredFileCategory::Portrait;
}
