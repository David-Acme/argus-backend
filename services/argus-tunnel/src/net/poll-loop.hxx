#pragma once

#include <sys/epoll.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Event-loop actor registered in the poll loop; owns no fd lifecycle itself.
class LoopActor : public std::enable_shared_from_this<LoopActor>
{
public:
  virtual ~LoopActor() = default;
  virtual void handleEvents(uint32_t events) = 0;
};

// Single-threaded epoll loop: level-triggered I/O, timers, cross-thread posts, deferred release.
class PollLoop
{
public:
  using Task = std::function<void()>;

  struct UpdateInput
  {
    int fd{-1};
    uint32_t events{0};
    LoopActor* actor{};
  };

  PollLoop();
  ~PollLoop();
  PollLoop(const PollLoop&) = delete;
  PollLoop& operator=(const PollLoop&) = delete;

  void run();
  void stop();
  void post(Task task);
  void runAfter(int milliseconds, Task task);

  void watch(int fd, LoopActor* actor);
  void update(const UpdateInput& input);
  void unwatch(int fd);

  // Keeps an object alive until the end of the loop iteration so a callback may destroy it safely.
  void retain(std::shared_ptr<void> object);

private:
  void runDueTimers();
  void runPostedTasks();
  int pollTimeoutMs() const;

  int epollFd_{-1};
  int wakeFd_{-1};
  bool running_{false};
  std::multimap<std::chrono::steady_clock::time_point, Task> timers_;
  std::vector<std::shared_ptr<void>> retained_;

  std::mutex postedMutex_;
  std::vector<Task> posted_;
};
