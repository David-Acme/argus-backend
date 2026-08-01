#include "thread-budget.hxx"

#include <algorithm>
#include <thread>

namespace ThreadBudget
{

int hardwareThreads()
{
  auto hw = std::thread::hardware_concurrency();
  return static_cast<int>(std::max(1u, hw));
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

} // namespace ThreadBudget
