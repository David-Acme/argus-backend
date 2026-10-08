#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

namespace turn
{

struct CalibrationPoint
{
  double lower{0.0};
  double upper{0.0};
  double value{0.0};
};

struct CalibrationModel
{
  std::string type{"identity"};
  double scale{1.0};
  double shift{0.0};
  std::vector<CalibrationPoint> points;

  [[nodiscard]] bool valid() const { return type == "identity" || type == "platt" || (type == "isotonic" && !points.empty()); }

  [[nodiscard]] double apply(double confidence) const;
};

[[nodiscard]] std::optional<CalibrationModel> calibrationFromJson(const Json::Value& node);

[[nodiscard]] double logitOf(double value);

[[nodiscard]] double sigmoidOf(double value);

}
