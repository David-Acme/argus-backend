#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-data-host.hxx>

#include <string>

class ProductivityModuleData final : public ModuleDataHost
{
public:
  ProductivityModuleData() = default;
  ~ProductivityModuleData() override = default;
  ProductivityModuleData(const ProductivityModuleData&) = delete;
  ProductivityModuleData& operator=(const ProductivityModuleData&) = delete;
  ProductivityModuleData(ProductivityModuleData&&) = delete;
  ProductivityModuleData& operator=(ProductivityModuleData&&) = delete;

  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override;
  ModuleDataPurge purge(const std::string& moduleId) override;

  [[nodiscard]] drogon::Task<ModuleDataSummary> summaryAsync(std::string moduleId) const;
  [[nodiscard]] drogon::Task<ModuleDataPurge> purgeAsync(std::string moduleId) const;

private:
  ModuleDataRepository repository_;
};
