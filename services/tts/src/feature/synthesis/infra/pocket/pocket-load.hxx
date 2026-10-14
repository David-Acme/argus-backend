#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

class PocketLoadStopped final : public std::runtime_error
{
public:
  explicit PocketLoadStopped(const std::string& component)
      : std::runtime_error("the Pocket load stopped before " + component)
  {
  }
};

struct PocketLoadStep
{
  std::string name;
  std::function<void()> load;
};

void runPocketLoad(const std::vector<PocketLoadStep>& steps,
                   const std::function<bool()>& stopRequested);
