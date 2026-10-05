#include "face-crop-store.hxx"

#include <drogon/drogon.h>
#include <exception>
#include <shared/repositories/pending-object-delete/pending-object-delete-repository.hxx>
#include <storage/s3-storage-service.hxx>

bool FaceCropStore::isConfigured() const
{
  return S3StorageService{}.isConfigured();
}

drogon::Task<std::optional<std::string>>
FaceCropStore::put(const FaceCropPutInput& input) const
{
  const S3StorageService storage;
  if (!storage.isConfigured() || input.jpeg.empty())
    co_return std::nullopt;
  const std::string key = "faces/" + std::to_string(input.personId) + "/" +
                          std::to_string(input.sampleId) + ".jpg";
  try {
    co_await storage.put(
        {.objectKey = key, .body = input.jpeg, .contentType = "image/jpeg"});
    co_return key;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Face crop storage failed for person " << input.personId << ": "
             << error.what();
  }
  co_return std::nullopt;
}

drogon::Task<std::optional<std::string>>
FaceCropStore::read(const std::string& key) const
{
  const S3StorageService storage;
  if (!storage.isConfigured() || key.empty())
    co_return std::nullopt;
  try {
    co_return co_await storage.get(key);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Face crop read failed: " << error.what();
  }
  co_return std::nullopt;
}

drogon::Task<void> FaceCropStore::remove(const std::string& key) const
{
  const S3StorageService storage;
  if (!storage.isConfigured() || key.empty())
    co_return;
  bool removed = false;
  try {
    co_await storage.remove(key);
    removed = true;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Face crop removal failed; it waits for the deletion worker: "
             << error.what();
  }
  if (removed)
    co_return;
  const PendingObjectEnqueueInput pending{.objectKeys = {key}, .client = nullptr};
  co_await pendingRepository_.enqueue(pending);
}
