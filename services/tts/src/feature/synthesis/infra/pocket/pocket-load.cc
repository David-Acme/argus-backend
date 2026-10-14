#include "pocket-load.hxx"

void runPocketLoad(const std::vector<PocketLoadStep>& steps,
                   const std::function<bool()>& stopRequested)
{
  for (const auto& step : steps) {
    if (stopRequested && stopRequested())
      throw PocketLoadStopped(step.name);
    step.load();
  }
}
