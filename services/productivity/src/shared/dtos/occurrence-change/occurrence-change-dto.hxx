#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>

struct OccurrenceChangeDto
{
  int64_t occurrence{0};
  bool cancelled{false};
  std::optional<int64_t> startsAt;
  std::optional<int64_t> endsAt;
  std::optional<std::string> title;
  std::optional<std::string> location;

  [[nodiscard]] bool changesMoreThanTheTime() const;

  static OccurrenceChangeDto fromJson(const Json::Value& json);
  static OccurrenceChangeDto validated(OccurrenceChangeDto dto);
};
