#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-data-host.hxx>
#include <storage/s3-storage-service.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

struct CameraModuleDataInput
{
  std::function<void(std::int64_t)> forgetCamera;
  std::function<bool()> storageReady;
  std::function<drogon::Task<void>(std::string)> removeObject;
  std::chrono::milliseconds objectBudget{3000};
};

class CameraModuleData final : public ModuleDataHost
{
public:
  CameraModuleData();
  explicit CameraModuleData(CameraModuleDataInput input);
  ~CameraModuleData() override = default;
  CameraModuleData(const CameraModuleData&) = delete;
  CameraModuleData& operator=(const CameraModuleData&) = delete;
  CameraModuleData(CameraModuleData&&) = delete;
  CameraModuleData& operator=(CameraModuleData&&) = delete;

  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override;
  ModuleDataPurge purge(const std::string& moduleId) override;

  [[nodiscard]] drogon::Task<ModuleDataSummary> summaryAsync(std::string moduleId) const;
  [[nodiscard]] drogon::Task<ModuleDataPurge> purgeAsync(std::string moduleId) const;

private:
  [[nodiscard]] drogon::Task<ModuleDataPurge> removeEvidenceObjects() const;

  CameraModuleDataInput input_;
  ModuleDataRepository repository_;
  S3StorageService storage_;
};
