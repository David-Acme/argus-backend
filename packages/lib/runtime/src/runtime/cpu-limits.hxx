#pragma once

#include <sched.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cpu_limits
{

using FileReader =
    std::function<std::optional<std::string>(const std::string& path)>;

struct CpuLimitInput
{
  int online{1};
  int affinity{0};
  std::optional<double> quotaCpus;
};

struct CgroupQuotaInput
{
  std::string_view procSelfCgroup;
  std::string_view mountRoot{"/sys/fs/cgroup"};
  FileReader readFile;
};

inline std::string_view trimmed(std::string_view text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

inline std::optional<long long> parseInteger(std::string_view text)
{
  text = trimmed(text);
  long long value = 0;
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end)
    return std::nullopt;
  return value;
}

inline std::optional<double> quotaRatio(long long quota, long long period)
{
  if (quota <= 0 || period <= 0)
    return std::nullopt;
  return static_cast<double>(quota) / static_cast<double>(period);
}

inline std::optional<double> parseCgroupV2CpuMax(std::string_view text)
{
  text = trimmed(text);
  const auto space = text.find(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  const std::string_view quota = text.substr(0, space);
  if (trimmed(quota) == "max")
    return std::nullopt;
  const auto quotaValue = parseInteger(quota);
  const auto periodValue = parseInteger(text.substr(space + 1));
  if (!quotaValue || !periodValue)
    return std::nullopt;
  return quotaRatio(*quotaValue, *periodValue);
}

inline std::optional<double> parseCgroupV1Quota(std::string_view quota,
                                                std::string_view period)
{
  const auto quotaValue = parseInteger(quota);
  const auto periodValue = parseInteger(period);
  if (!quotaValue || !periodValue)
    return std::nullopt;
  return quotaRatio(*quotaValue, *periodValue);
}

inline std::optional<double> tighter(std::optional<double> current,
                                     std::optional<double> candidate)
{
  if (!candidate)
    return current;
  if (!current)
    return candidate;
  return std::min(*current, *candidate);
}

inline std::vector<std::string> ancestorsOf(std::string_view path)
{
  std::vector<std::string> chain;
  std::string current(trimmed(path));
  while (!current.empty() && current.back() == '/')
    current.pop_back();
  for (;;) {
    chain.push_back(current);
    const auto slash = current.rfind('/');
    if (slash == std::string::npos || current.empty())
      break;
    current.resize(slash);
  }
  if (!chain.back().empty())
    chain.emplace_back();
  return chain;
}

inline bool listsController(std::string_view controllers,
                            std::string_view wanted)
{
  while (!controllers.empty()) {
    const auto comma = controllers.find(',');
    if (controllers.substr(0, comma) == wanted)
      return true;
    if (comma == std::string_view::npos)
      break;
    controllers.remove_prefix(comma + 1);
  }
  return false;
}

inline std::optional<double> v2Quota(const CgroupQuotaInput& input,
                                     std::string_view path)
{
  std::optional<double> quota;
  for (const auto& ancestor : ancestorsOf(path)) {
    const auto text = input.readFile(std::string(input.mountRoot) + ancestor +
                                     "/cpu.max");
    if (text)
      quota = tighter(quota, parseCgroupV2CpuMax(*text));
  }
  return quota;
}

inline std::optional<double> v1Quota(const CgroupQuotaInput& input,
                                     std::string_view controllers,
                                     std::string_view path)
{
  const std::vector<std::string> mounts = {std::string(controllers), "cpu",
                                           "cpu,cpuacct", "cpuacct,cpu"};
  std::optional<double> quota;
  for (const auto& mount : mounts) {
    const std::string base = std::string(input.mountRoot) + "/" + mount;
    for (const auto& ancestor : ancestorsOf(path)) {
      const auto quotaText = input.readFile(base + ancestor + "/cpu.cfs_quota_us");
      const auto periodText =
          input.readFile(base + ancestor + "/cpu.cfs_period_us");
      if (quotaText && periodText)
        quota = tighter(quota, parseCgroupV1Quota(*quotaText, *periodText));
    }
  }
  return quota;
}

inline std::optional<double> cgroupQuota(const CgroupQuotaInput& input)
{
  std::optional<double> quota;
  std::istringstream lines{std::string(input.procSelfCgroup)};
  std::string line;
  while (std::getline(lines, line)) {
    const auto first = line.find(':');
    const auto second =
        first == std::string::npos ? first : line.find(':', first + 1);
    if (second == std::string::npos)
      continue;
    const std::string_view view(line);
    const std::string_view controllers = view.substr(first + 1, second - first - 1);
    const std::string_view path = view.substr(second + 1);
    if (view.substr(0, first) == "0" && controllers.empty())
      quota = tighter(quota, v2Quota(input, path));
    else if (listsController(controllers, "cpu"))
      quota = tighter(quota, v1Quota(input, controllers, path));
  }
  return quota;
}

inline int effectiveThreads(const CpuLimitInput& input)
{
  int threads = input.online > 0 ? input.online : std::max(1, input.affinity);
  if (input.affinity > 0)
    threads = std::min(threads, input.affinity);
  if (input.quotaCpus && *input.quotaCpus > 0.0)
    threads = std::min(threads,
                       std::max(1, static_cast<int>(std::ceil(*input.quotaCpus))));
  return std::max(1, threads);
}

inline std::optional<std::string> readSmallFile(const std::string& path)
{
  std::ifstream in(path);
  if (!in.is_open())
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

inline int affinityThreads()
{
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0)
    return 0;
  return CPU_COUNT(&set);
}

inline int probeEffectiveThreads()
{
  const auto procSelf = readSmallFile("/proc/self/cgroup");
  return effectiveThreads(
      {.online = static_cast<int>(std::thread::hardware_concurrency()),
       .affinity = affinityThreads(),
       .quotaCpus = procSelf ? cgroupQuota({.procSelfCgroup = *procSelf,
                                            .mountRoot = "/sys/fs/cgroup",
                                            .readFile = readSmallFile})
                             : std::nullopt});
}

}
