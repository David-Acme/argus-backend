#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

struct CapabilityRecordInput
{
  int64_t cameraId{0};
  std::string capabilities{};
};

namespace capability_record
{
drogon::Task<bool> record(CapabilityRecordInput input);
void learned(CapabilityRecordInput input);
void discover(std::vector<int64_t> cameraIds);
}
