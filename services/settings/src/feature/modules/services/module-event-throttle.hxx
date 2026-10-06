#pragma once

#include <feature/modules/schemas/module-job.hxx>

#include <cstdint>
#include <map>

struct ThrottleSample
{
  std::int64_t jobId{0};
  JobState state{JobState::Queued};
  double progress{0};
  std::int64_t nowMs{0};
};

class ModuleEventThrottle
{
public:
  static constexpr std::int64_t kMinIntervalMs = 1000;
  static constexpr double kMinStep = 0.01;

  [[nodiscard]] bool admit(const ThrottleSample& sample);

private:
  struct Last
  {
    JobState state{JobState::Queued};
    double progress{0};
    std::int64_t atMs{0};
  };

  std::map<std::int64_t, Last> last_;
};
