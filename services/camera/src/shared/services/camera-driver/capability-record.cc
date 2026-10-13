#include <shared/services/camera-driver/capability-record.hxx>

#include <shared/services/camera-driver/camera-driver.hxx>

#include <drogon/utils/coroutine.h>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/camera-change-sink.hxx>
#include <trantor/utils/Logger.h>

#include <exception>
#include <optional>
#include <utility>

drogon::Task<void> CapabilityRecorder::recordQuietly(CapabilityRecordInput input) const
{
  const int64_t cameraId = input.cameraId;
  try {
    co_await record(std::move(input));
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera capabilities: could not record what camera " << cameraId
             << " reported (" << error.what() << ")";
  }
}

drogon::Task<void> CapabilityRecorder::discoverAll(std::vector<int64_t> cameraIds) const
{
  for (const int64_t cameraId : cameraIds) {
    try {
      const auto camera = co_await repository_.findById(cameraId);
      if (!camera || camera->driver != CameraDriver::Onvif || !camera->isEnabled)
        continue;
      const auto driver = CameraDriverRegistry::instance().controlDriverFor(*camera);
      if (!driver)
        continue;
      static_cast<void>(co_await BlockingTask<Json::Value>([driver]() { return driver->discoverCapabilities(); }));
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera capabilities: could not ask camera " << cameraId << " (" << error.what() << ")";
    }
  }
}

drogon::Task<bool> CapabilityRecorder::record(CapabilityRecordInput input) const
{
  const auto client = DbService::cameraClient();
  if (!client)
    co_return false;
  auto transaction = co_await db_transaction::begin(client);
  try {
    const auto before = co_await repository_.findById(input.cameraId, transaction.get());
    if (!before || before->capabilities == input.capabilities) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    CameraUpdateInput change;
    change.capabilities = std::move(input.capabilities);
    change.client = transaction.get();
    const CameraSchema row = co_await repository_.update(input.cameraId, change);
    if (row.id == 0) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    if (const CameraChangeSink* sink = camera_change::getSink(); sink != nullptr)
      co_await sink->publishAudit({.recordId = row.id,
                                   .tableName = TableName::Camera,
                                   .before = before->toJson(),
                                   .after = row.toJson(),
                                   .actorId = std::nullopt,
                                   .client = transaction.get()});
    else
      LOG_WARN << "camera change sink not installed; drop camera audit";
    co_return co_await db_transaction::Commit(std::move(transaction));
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
}

void CapabilityRecorder::learned(CapabilityRecordInput input) const
{
  if (input.cameraId <= 0)
    return;
  drogon::async_run([this, input = std::move(input)]() mutable { return recordQuietly(std::move(input)); });
}

void CapabilityRecorder::discover(std::vector<int64_t> cameraIds) const
{
  if (cameraIds.empty())
    return;
  drogon::async_run(
      [this, cameraIds = std::move(cameraIds)]() mutable { return discoverAll(std::move(cameraIds)); });
}
