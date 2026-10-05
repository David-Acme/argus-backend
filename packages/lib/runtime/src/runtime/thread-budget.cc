#include "thread-budget.hxx"

#include "cpu-limits.hxx"

#include <algorithm>

namespace ThreadBudget
{

int hardwareThreads()
{
  static const int threads = cpu_limits::probeEffectiveThreads();
  return threads;
}

int computeThreads()
{
  return std::clamp(hardwareThreads() / 2, 2, 16);
}

int batchThreads()
{
  return std::clamp(hardwareThreads() / 2, 4, 16);
}

int heavyThreads()
{
  return std::clamp(hardwareThreads() * 3 / 4, 2, 12);
}

int lightThreads()
{
  return std::clamp(hardwareThreads() / 4, 2, 8);
}

int inferenceSlots()
{
  return std::clamp(hardwareThreads() / 8, 1, 4);
}

int blockingLightThreads()
{
  return std::clamp(hardwareThreads() * 4, 16, 64);
}

int blockingHeavyThreads()
{
  return std::clamp(hardwareThreads(), 4, 16);
}

int ttsThreads()
{
  return std::clamp(hardwareThreads() / 2, 2, 8);
}

int extractionSlots()
{
  return std::clamp(hardwareThreads() / 8, 1, 4);
}

int extractionThreads()
{
  return std::clamp(hardwareThreads() / 4, 1, 4);
}

int queueWorkers(const std::string& queueName)
{
  if (queueName == "memory.extract")
    return extractionSlots();
  return 1;
}

}
