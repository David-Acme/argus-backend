#pragma once

#include <feature/module-data/repositories/module-data/module-data-repository.hxx>
#include <shared/repositories/pending-object-delete/pending-object-delete-repository.hxx>

#include <drogon/utils/coroutine.h>
#include <settings/module-data-host.hxx>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct IdentityModuleDataInput
{
  std::function<void(const std::vector<std::int64_t>&)> forgetVectors;
  std::function<void()> kickDeletion;
};

class IdentityModuleData final : public ModuleDataHost
{
public:
  IdentityModuleData();
  explicit IdentityModuleData(IdentityModuleDataInput input);
  ~IdentityModuleData() override = default;
  IdentityModuleData(const IdentityModuleData&) = delete;
  IdentityModuleData& operator=(const IdentityModuleData&) = delete;
  IdentityModuleData(IdentityModuleData&&) = delete;
  IdentityModuleData& operator=(IdentityModuleData&&) = delete;

  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override;
  ModuleDataPurge purge(const std::string& moduleId) override;

  [[nodiscard]] drogon::Task<ModuleDataSummary> summaryAsync(std::string moduleId) const;
  [[nodiscard]] drogon::Task<ModuleDataPurge> purgeAsync(std::string moduleId) const;

private:
  IdentityModuleDataInput input_;
  ModuleDataRepository repository_;
  PendingObjectDeleteRepository pendingRepository_;
};
