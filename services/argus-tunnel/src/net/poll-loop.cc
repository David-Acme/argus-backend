#include "poll-loop.hxx"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace
{
constexpr uint32_t kWakeEvents = EPOLLIN;
}

PollLoop::PollLoop()
{
  epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epollFd_ < 0)
    throw std::runtime_error("epoll_create1 failed");
  wakeFd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wakeFd_ < 0) {
    ::close(epollFd_);
    epollFd_ = -1;
    throw std::runtime_error("eventfd failed");
  }
  epoll_event event{};
  event.events = kWakeEvents;
  event.data.ptr = nullptr;
  if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &event) < 0) {
    ::close(wakeFd_);
    ::close(epollFd_);
    epollFd_ = -1;
    wakeFd_ = -1;
    throw std::runtime_error("epoll_ctl wake failed");
  }
}

PollLoop::~PollLoop()
{
  if (epollFd_ >= 0)
    ::close(epollFd_);
  if (wakeFd_ >= 0)
    ::close(wakeFd_);
}

void PollLoop::watch(int fd, LoopActor* actor)
{
  epoll_event event{};
  event.events = EPOLLIN | EPOLLRDHUP;
  event.data.ptr = actor;
  if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) < 0)
    throw std::runtime_error("epoll_ctl add failed");
}

void PollLoop::update(const UpdateInput& input)
{
  epoll_event event{};
  event.events = input.events | EPOLLRDHUP;
  event.data.ptr = input.actor;
  if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, input.fd, &event) < 0)
    throw std::runtime_error("epoll_ctl mod failed");
}

void PollLoop::unwatch(int fd)
{
  ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
}

void PollLoop::retain(std::shared_ptr<void> object)
{
  retained_.push_back(std::move(object));
}

void PollLoop::post(Task task)
{
  {
    std::lock_guard<std::mutex> lock(postedMutex_);
    posted_.push_back(std::move(task));
  }
  const uint64_t one = 1;
  ssize_t written = ::write(wakeFd_, &one, sizeof(one));
  (void)written;
}

void PollLoop::runAfter(int milliseconds, Task task)
{
  timers_.emplace(std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(milliseconds),
                  std::move(task));
}


void PollLoop::stop()
{
  post([this] { running_ = false; });
}

void PollLoop::run()
{
  running_ = true;
  std::vector<epoll_event> events(64);
  while (running_) {
    const int count =
        ::epoll_wait(epollFd_, events.data(), static_cast<int>(events.size()),
                     pollTimeoutMs());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    for (int i = 0; i < count; ++i) {
      if (events[i].data.ptr == nullptr) {
        uint64_t value = 0;
        ssize_t ignored = ::read(wakeFd_, &value, sizeof(value));
        (void)ignored;
        runPostedTasks();
      } else {
        static_cast<LoopActor*>(events[i].data.ptr)
            ->handleEvents(events[i].events);
      }
    }
    runDueTimers();
    retained_.clear();
  }
  runPostedTasks();
  retained_.clear();
}

void PollLoop::runPostedTasks()
{
  std::vector<Task> batch;
  {
    std::lock_guard<std::mutex> lock(postedMutex_);
    batch.swap(posted_);
  }
  for (auto& task : batch)
    task();
}

void PollLoop::runDueTimers()
{
  if (timers_.empty())
    return;
  const auto now = std::chrono::steady_clock::now();
  std::vector<Task> due;
  for (auto it = timers_.begin(); it != timers_.end() && it->first <= now;) {
    due.push_back(std::move(it->second));
    it = timers_.erase(it);
  }
  for (auto& task : due)
    task();
}


int PollLoop::pollTimeoutMs() const
{
  if (timers_.empty())
    return 1000;
  const auto next = timers_.begin()->first;
  const auto now = std::chrono::steady_clock::now();
  if (next <= now)
    return 0;
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
  return static_cast<int>(ms.count()) + 1;
}
