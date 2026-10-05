#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

struct FaceCropPutInput
{
  int64_t personId{0};
  int64_t sampleId{0};
  const std::string& jpeg;
};

class FaceCropStore
{
public:
  [[nodiscard]] bool isConfigured() const;
  drogon::Task<std::optional<std::string>> put(const FaceCropPutInput& input) const;
  drogon::Task<std::optional<std::string>> read(const std::string& key) const;
  drogon::Task<void> remove(const std::string& key) const;
};
