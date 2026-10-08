#include "eval-report.hxx"

#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

namespace eval
{

Interval wilson(const Counts& counts)
{
  if (counts.total <= 0)
    return {};
  constexpr double kZ = 1.96;
  const double n = counts.total;
  const double p = counts.hits / n;
  const double denominator = 1.0 + kZ * kZ / n;
  const double centre = (p + kZ * kZ / (2.0 * n)) / denominator;
  const double half = kZ * std::sqrt(p * (1.0 - p) / n + kZ * kZ / (4.0 * n * n)) / denominator;
  return {.lower = std::max(0.0, centre - half), .upper = std::min(1.0, centre + half)};
}

LoadedGates loadGates(const std::string& path, const std::string& section)
{
  LoadedGates loaded;
  std::ifstream in(path);
  if (!in) {
    loaded.error = "cannot open " + path;
    return loaded;
  }
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  if (!Json::parseFromStream(builder, in, &root, &errors)) {
    loaded.error = path + ": " + errors;
    return loaded;
  }
  const Json::Value& metrics = root[section]["metrics"];
  if (!metrics.isObject()) {
    loaded.error = path + ": no gated metrics in section '" + section + "'";
    return loaded;
  }
  for (const auto& name : metrics.getMemberNames()) {
    Gate gate{.metric = name, .min = std::nullopt, .max = std::nullopt};
    if (metrics[name].isMember("min"))
      gate.min = metrics[name]["min"].asDouble();
    if (metrics[name].isMember("max"))
      gate.max = metrics[name]["max"].asDouble();
    loaded.gates.push_back(std::move(gate));
  }
  return loaded;
}

Verdict check(const std::vector<Gate>& gates, const Metrics& metrics)
{
  Verdict verdict;
  for (const auto& gate : gates) {
    const auto found = metrics.find(gate.metric);
    if (found == metrics.end()) {
      verdict.failures.push_back(gate.metric + ": gated but not measured");
      continue;
    }
    if (gate.min && found->second < *gate.min - 1e-9)
      verdict.failures.push_back(gate.metric + ": " + std::to_string(found->second) + " below " +
                                 std::to_string(*gate.min));
    if (gate.max && found->second > *gate.max + 1e-9)
      verdict.failures.push_back(gate.metric + ": " + std::to_string(found->second) + " above " +
                                 std::to_string(*gate.max));
  }
  return verdict;
}

bool writeReport(const std::string& path, const ReportInput& input)
{
  Json::Value root(Json::objectValue);
  root["section"] = input.section;
  Json::Value metrics(Json::objectValue);
  for (const auto& [name, value] : input.metrics)
    metrics[name] = value;
  root["metrics"] = metrics;
  Json::Value failures(Json::arrayValue);
  for (const auto& failure : input.verdict.failures)
    failures.append(failure);
  root["passed"] = input.verdict.passed();
  root["pinned"] = input.pinned;
  root["failures"] = std::move(failures);
  std::ofstream out(path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  out << Json::writeString(builder, root) << "\n";
  return static_cast<bool>(out);
}

void printMetrics(const Metrics& metrics)
{
  for (const auto& [name, value] : metrics)
    std::printf("  %-52s %.4f\n", name.c_str(), value);
}

}
