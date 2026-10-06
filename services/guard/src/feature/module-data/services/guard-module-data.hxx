#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-data-host.hxx>
#include <storage/s3-storage-service.hxx>

#include <chrono>
#include <functional>
#include <string>

struct GuardModuleDataInput
{
  std::function<bool()> storageReady;
  std::function<drogon::Task<void>(std::string)> removeObject;
  std::chrono::milliseconds objectBudget{3000};
};

class GuardModuleData final : public ModuleDataHost
{
public:
  GuardModuleData();
  explicit GuardModuleData(GuardModuleDataInput input);
  ~GuardModuleData() override = default;
  GuardModuleData(const GuardModuleData&) = delete;
  GuardModuleData& operator=(const GuardModuleData&) = delete;
  GuardModuleData(GuardModuleData&&) = delete;
  GuardModuleData& operator=(GuardModuleData&&) = delete;

  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override;
  ModuleDataPurge purge(const std::string& moduleId) override;

  [[nodiscard]] drogon::Task<ModuleDataSummary> summaryAsync(std::string moduleId) const;
  [[nodiscard]] drogon::Task<ModuleDataPurge> purgeAsync(std::string moduleId) const;

private:
  [[nodiscard]] drogon::Task<ModuleDataPurge> removeEvidenceObjects() const;

  GuardModuleDataInput input_;
  ModuleDataRepository repository_;
  S3StorageService storage_;
};
