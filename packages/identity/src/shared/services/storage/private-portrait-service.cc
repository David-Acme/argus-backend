#include "private-portrait-service.hxx"

#include <shared/services/storage/s3-storage-service.hxx>

#include <drogon/drogon.h>
#include <optional>
#include <stdexcept>
#include <shared/repositories/stored-file/stored-file-repository.hxx>
#include <shared/repositories/user-portrait/user-portrait-repository.hxx>

drogon::Task<void>
PrivatePortraitService::store(int64_t userId, const std::string& image) const
{
  S3StorageService storage;
  if (!storage.isConfigured()) {
    LOG_WARN << "Portrait storage is not configured; skipping portrait for user "
             << userId;
    co_return;
  }

  std::optional<S3StoredObject> object;
  std::string failure;
  try {
    object = co_await storage.putPortrait(userId, image);
    StoredFileRepository files;
    const auto file = co_await files.create({
        .objectKey = object->objectKey,
        .sha256 = object->sha256,
        .mimeType = "image/jpeg",
        .byteSize = object->byteSize,
        .category = StoredFileCategory::Portrait,
        .createdBy = userId,
    });
    UserPortraitRepository portraits;
    co_await portraits.upsertCurrent({.userId = userId, .fileId = file.id});
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
  UserPortraitRepository portraits;
  const auto portrait = co_await portraits.findByUserId(userId);
  if (!portrait)
    co_return std::nullopt;

  StoredFileRepository files;
  const auto file = co_await files.findById(portrait->fileId);
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
  UserPortraitRepository portraits;
  const auto portrait = co_await portraits.findByUserId(userId);
  if (!portrait)
    co_return false;

  StoredFileRepository files;
  const auto file = co_await files.findById(portrait->fileId);
  co_return file && file->category == StoredFileCategory::Portrait;
}
