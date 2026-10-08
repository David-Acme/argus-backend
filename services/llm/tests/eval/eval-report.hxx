#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace eval
{

using Metrics = std::map<std::string, double>;

struct Interval
{
  double lower = 0.0;
  double upper = 0.0;
};

struct Counts
{
  int hits = 0;
  int total = 0;
};

Interval wilson(const Counts& counts);

struct Gate
{
  std::string metric;
  std::optional<double> min;
  std::optional<double> max;
};

struct LoadedGates
{
  std::vector<Gate> gates;
  std::string error;
};

LoadedGates loadGates(const std::string& path, const std::string& section);

struct Verdict
{
  std::vector<std::string> failures;
  [[nodiscard]] bool passed() const { return failures.empty(); }
};

Verdict check(const std::vector<Gate>& gates, const Metrics& metrics);

struct ReportInput
{
  std::string section;
  Metrics metrics;
  Verdict verdict;
  bool pinned{true};
};

bool writeReport(const std::string& path, const ReportInput& input);

void printMetrics(const Metrics& metrics);

}
