#pragma once

#include <feature/modules/infra/component-owners.hxx>
#include <feature/modules/schemas/module-catalog.hxx>
#include <feature/modules/schemas/module-job.hxx>
#include <feature/modules/schemas/module-role-move.hxx>
#include <feature/modules/schemas/module-state.hxx>
#include <feature/modules/services/hardware-check.hxx>

#include <cstdint>
#include <optional>
#include <vector>

struct ComponentView
{
  const CatalogComponent* component{nullptr};
  ComponentStatus status;
  OwnerReach reach{OwnerReach::Unreachable};
};

struct JobView
{
  ModuleJobSchema job;
  double progress{0};
  std::int64_t bytesPerSecond{0};
  std::optional<std::int64_t> etaSeconds;
  std::vector<ModuleRoleMove> roleMoves{};
};

struct ModuleView
{
  const CatalogModule* module{nullptr};
  bool enabled{false};
  ModuleLifecycle lifecycle{ModuleLifecycle::NotInstalled};
  bool hasData{false};
  std::int64_t dataPurgedAt{0};
  std::int64_t sizeBytes{0};
  std::int64_t installedBytes{0};
  HardwareAssessment hardware;
  std::optional<JobView> job;
  std::vector<ComponentView> components;
};
