#pragma once

#include <shared/repositories/camera/camera-repository.hxx>

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

struct CapabilityRecordInput
{
  int64_t cameraId{0};
  std::string capabilities{};
};

class CapabilityRecorder
{
public:
  [[nodiscard]] drogon::Task<bool> record(CapabilityRecordInput input) const;
  void learned(CapabilityRecordInput input) const;
  void discover(std::vector<int64_t> cameraIds) const;

private:
  [[nodiscard]] drogon::Task<void> recordQuietly(CapabilityRecordInput input) const;
  [[nodiscard]] drogon::Task<void> discoverAll(std::vector<int64_t> cameraIds) const;

  CameraRepository repository_;
};
