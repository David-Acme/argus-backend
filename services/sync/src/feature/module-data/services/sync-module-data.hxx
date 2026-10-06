#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-data-host.hxx>
#include <sync/table-name.hxx>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SyncModuleData final : public ModuleDataHost
{
public:
  SyncModuleData() = default;
  ~SyncModuleData() override = default;
  SyncModuleData(const SyncModuleData&) = delete;
  SyncModuleData& operator=(const SyncModuleData&) = delete;
  SyncModuleData(SyncModuleData&&) = delete;
  SyncModuleData& operator=(SyncModuleData&&) = delete;

  [[nodiscard]] static std::span<const TableName> tablesOf(std::string_view moduleId);

  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override;
  ModuleDataPurge purge(const std::string& moduleId) override;

  [[nodiscard]] drogon::Task<ModuleDataSummary> summaryAsync(std::string moduleId) const;
  [[nodiscard]] drogon::Task<ModuleDataPurge> purgeAsync(std::string moduleId) const;

private:
  ModuleDataRepository repository_;
};
