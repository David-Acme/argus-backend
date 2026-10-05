#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <runtime/cpu-limits.hxx>
#include <runtime/thread-budget.hxx>

#include <map>
#include <optional>
#include <string>
#include <thread>

namespace
{

cpu_limits::FileReader readerOf(const std::map<std::string, std::string>& files)
{
  return [files](const std::string& path) -> std::optional<std::string> {
    const auto it = files.find(path);
    if (it == files.end())
      return std::nullopt;
    return it->second;
  };
}

}

TEST_CASE("cgroup v2 cpu.max parses a quota and ignores max")
{
  CHECK(cpu_limits::parseCgroupV2CpuMax("150000 100000\n") == doctest::Approx(1.5));
  CHECK(cpu_limits::parseCgroupV2CpuMax("400000 100000") == doctest::Approx(4.0));
  CHECK_FALSE(cpu_limits::parseCgroupV2CpuMax("max 100000\n").has_value());
  CHECK_FALSE(cpu_limits::parseCgroupV2CpuMax("").has_value());
  CHECK_FALSE(cpu_limits::parseCgroupV2CpuMax("garbage").has_value());
  CHECK_FALSE(cpu_limits::parseCgroupV2CpuMax("0 100000").has_value());
}

TEST_CASE("cgroup v1 cfs quota treats -1 as unlimited")
{
  CHECK(cpu_limits::parseCgroupV1Quota("200000\n", "100000\n") ==
        doctest::Approx(2.0));
  CHECK_FALSE(cpu_limits::parseCgroupV1Quota("-1\n", "100000\n").has_value());
  CHECK_FALSE(cpu_limits::parseCgroupV1Quota("50000", "0").has_value());
}

TEST_CASE("the effective thread count is the tightest of online, affinity and quota")
{
  CHECK(cpu_limits::effectiveThreads({.online = 16, .affinity = 0, .quotaCpus = 1.5}) == 2);
  CHECK(cpu_limits::effectiveThreads({.online = 16, .affinity = 4, .quotaCpus = 2.5}) == 3);
  CHECK(cpu_limits::effectiveThreads({.online = 16, .affinity = 2, .quotaCpus = 8.0}) == 2);
  CHECK(cpu_limits::effectiveThreads({.online = 16, .affinity = 16, .quotaCpus = std::nullopt}) == 16);
  CHECK(cpu_limits::effectiveThreads({.online = 4, .affinity = 0, .quotaCpus = 0.25}) == 1);
  CHECK(cpu_limits::effectiveThreads({.online = 0, .affinity = 6, .quotaCpus = std::nullopt}) == 6);
  CHECK(cpu_limits::effectiveThreads({.online = 0, .affinity = 0, .quotaCpus = std::nullopt}) == 1);
}

TEST_CASE("a container's own cgroup v2 root carries its compose cpus limit")
{
  const auto quota = cpu_limits::cgroupQuota(
      {.procSelfCgroup = "0::/\n",
       .mountRoot = "/sys/fs/cgroup",
       .readFile = readerOf({{"/sys/fs/cgroup/cpu.max", "150000 100000\n"}})});
  REQUIRE(quota.has_value());
  CHECK(*quota == doctest::Approx(1.5));
}

TEST_CASE("a nested cgroup v2 path takes the tightest ancestor")
{
  const auto quota = cpu_limits::cgroupQuota(
      {.procSelfCgroup = "0::/system.slice/docker-abc.scope\n",
       .mountRoot = "/sys/fs/cgroup",
       .readFile = readerOf(
           {{"/sys/fs/cgroup/system.slice/docker-abc.scope/cpu.max", "max 100000"},
            {"/sys/fs/cgroup/system.slice/cpu.max", "300000 100000"}})});
  REQUIRE(quota.has_value());
  CHECK(*quota == doctest::Approx(3.0));
}

TEST_CASE("an unlimited cgroup v2 tree yields no quota")
{
  const auto quota = cpu_limits::cgroupQuota(
      {.procSelfCgroup = "0::/user.slice/session-1.scope\n",
       .mountRoot = "/sys/fs/cgroup",
       .readFile = readerOf(
           {{"/sys/fs/cgroup/user.slice/session-1.scope/cpu.max", "max 100000"}})});
  CHECK_FALSE(quota.has_value());
}

TEST_CASE("a cgroup v1 container reads the cfs quota at its mount root")
{
  const auto quota = cpu_limits::cgroupQuota(
      {.procSelfCgroup = "12:memory:/docker/abc\n4:cpu,cpuacct:/docker/abc\n"
                         "1:name=systemd:/docker/abc\n",
       .mountRoot = "/sys/fs/cgroup",
       .readFile = readerOf(
           {{"/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us", "200000\n"},
            {"/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us", "100000\n"}})});
  REQUIRE(quota.has_value());
  CHECK(*quota == doctest::Approx(2.0));
}

TEST_CASE("the probed budget never exceeds the online processor count")
{
  const int online = static_cast<int>(std::thread::hardware_concurrency());
  const int probed = cpu_limits::probeEffectiveThreads();
  CHECK(probed >= 1);
  if (online > 0)
    CHECK(probed <= online);
  CHECK(ThreadBudget::hardwareThreads() == probed);
}
