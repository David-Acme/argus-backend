#pragma once

#include <feature/llm/services/tools/tool-registry.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct ToolDirectoryPace
{
  std::chrono::milliseconds retryFloor{2000};
  std::chrono::milliseconds retryCeiling{30000};
  std::chrono::milliseconds refreshEvery{300000};
};

class ToolDirectory
{
public:
  ToolDirectory(ToolRegistry& registry, ToolDirectoryPace pace);
  ~ToolDirectory();

  ToolDirectory(const ToolDirectory&) = delete;
  ToolDirectory& operator=(const ToolDirectory&) = delete;

  void start();
  void requestRefresh();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  void run(const std::stop_token& stop);

  ToolRegistry& registry_;
  ToolDirectoryPace pace_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  bool refreshRequested_{false};
  std::atomic<bool> running_{false};
  std::jthread worker_;
};
