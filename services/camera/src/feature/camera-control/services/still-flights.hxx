#pragma once

#include <shared/services/camera-driver/camera-driver.hxx>

#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trantor
{
class EventLoop;
}

class StillFlights
{
public:
  struct Flight
  {
    int64_t cameraId{0};
    std::mutex mutex;
    bool done{false};
    DriverResult result;
    std::vector<std::pair<std::coroutine_handle<>, trantor::EventLoop*>> waiters;
  };

  class Wait
  {
  public:
    explicit Wait(std::shared_ptr<Flight> flight);

    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> handle);
    [[nodiscard]] DriverResult await_resume() const;

  private:
    std::shared_ptr<Flight> flight_;
  };

  class Ticket
  {
  public:
    Ticket(StillFlights* leader, std::shared_ptr<Flight> flight);
    ~Ticket();

    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    Ticket(Ticket&&) = delete;
    Ticket& operator=(Ticket&&) = delete;

    [[nodiscard]] bool leads() const;
    [[nodiscard]] Wait result() const;
    void land(const DriverResult& result);

  private:
    StillFlights* leader_;
    std::shared_ptr<Flight> flight_;
  };

  [[nodiscard]] Ticket join(int64_t cameraId);

private:
  void finish(Flight& flight, const DriverResult& result);

  std::mutex mutex_;
  std::unordered_map<int64_t, std::shared_ptr<Flight>> flights_;
};
