#pragma once

#include "environment-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class EnvironmentRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<GuardEnvironment>> list() const;

  [[nodiscard]] drogon::Task<std::optional<GuardEnvironment>>
  find(int64_t id) const;

  [[nodiscard]] drogon::Task<std::optional<GuardEnvironmentScope>>
  forCamera(int64_t cameraId) const;

  [[nodiscard]] drogon::Task<int64_t> count() const;

  [[nodiscard]] drogon::Task<int64_t> defaultId() const;

  [[nodiscard]] drogon::Task<GuardEnvironment>
  create(const EnvironmentCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<GuardEnvironment>>
  update(const EnvironmentUpdateInput& input) const;

  [[nodiscard]] drogon::Task<int64_t>
  setMode(const EnvironmentModeInput& input) const;

  [[nodiscard]] drogon::Task<EnvironmentRemoval>
  remove(const EnvironmentRemoveInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<CameraAssignment>>
  cameraAssignments() const;
};
