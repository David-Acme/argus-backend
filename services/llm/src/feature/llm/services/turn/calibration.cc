#include "calibration.hxx"

#include <algorithm>
#include <cmath>

namespace turn
{

namespace
{
constexpr double kEpsilon = 1e-6;
}

double logitOf(double value)
{
  const double clipped = std::min(1.0 - kEpsilon, std::max(kEpsilon, value));
  return std::log(clipped / (1.0 - clipped));
}

double sigmoidOf(double value)
{
  if (value >= 0.0)
    return 1.0 / (1.0 + std::exp(-value));
  const double e = std::exp(value);
  return e / (1.0 + e);
}

double CalibrationModel::apply(double confidence) const
{
  if (type == "platt")
    return sigmoidOf(scale * logitOf(confidence) + shift);
  if (type != "isotonic" || points.empty())
    return confidence;
  if (confidence <= points.front().lower)
    return points.front().value;
  if (confidence >= points.back().upper)
    return points.back().value;
  std::size_t low = 0;
  std::size_t high = points.size() - 1;
  while (low < high) {
    const std::size_t middle = (low + high + 1) / 2;
    if (points[middle].lower <= confidence)
      low = middle;
    else
      high = middle - 1;
  }
  if (confidence <= points[low].upper || low == points.size() - 1)
    return points[low].value;
  const double span = points[low + 1].lower - points[low].upper;
  const double weight = span > 0.0 ? (confidence - points[low].upper) / span : 0.0;
  return points[low].value + weight * (points[low + 1].value - points[low].value);
}

std::optional<CalibrationModel> calibrationFromJson(const Json::Value& node)
{
  if (!node.isObject())
    return std::nullopt;
  CalibrationModel model;
  model.type = node.get("type", "identity").asString();
  if (model.type == "identity")
    return model;
  if (model.type == "platt") {
    model.scale = node.get("scale", 1.0).asDouble();
    model.shift = node.get("shift", 0.0).asDouble();
    return model.valid() ? std::optional<CalibrationModel>(std::move(model)) : std::nullopt;
  }
  if (model.type == "isotonic") {
    const Json::Value& lower = node["lower"];
    const Json::Value& upper = node["upper"];
    const Json::Value& value = node["value"];
    if (!lower.isArray() || !upper.isArray() || !value.isArray() || lower.size() != upper.size() || lower.size() != value.size())
      return std::nullopt;
    for (Json::ArrayIndex index = 0; index < lower.size(); ++index)
      model.points.push_back({.lower = lower[index].asDouble(), .upper = upper[index].asDouble(), .value = value[index].asDouble()});
    return model.valid() ? std::optional<CalibrationModel>(std::move(model)) : std::nullopt;
  }
  return std::nullopt;
}

}
