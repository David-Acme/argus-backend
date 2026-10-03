#pragma once

#include <cstddef>
#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>

struct VoiceSampleDto
{
  static constexpr size_t kMaxEncodedBytes = size_t{8} * 1024 * 1024;

  std::string audio;
  std::string challengeId;
  int64_t phrase{0};

  [[nodiscard]] std::string wav() const;

  static VoiceSampleDto fromJson(const Json::Value& json);
};
