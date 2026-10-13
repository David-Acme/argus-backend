#include <feature/camera-control/services/still-flights.hxx>

#include <runtime/blocking-task.hxx>
#include <trantor/net/EventLoop.h>

StillFlights::Wait::Wait(std::shared_ptr<Flight> flight) : flight_(std::move(flight)) {}

bool StillFlights::Wait::await_ready() const
{
  const std::scoped_lock lock(flight_->mutex);
  return flight_->done;
}

bool StillFlights::Wait::await_suspend(std::coroutine_handle<> handle)
{
  trantor::EventLoop* loop =
      blocking_task::resumeLoopFor(trantor::EventLoop::getEventLoopOfCurrentThread());
  const std::scoped_lock lock(flight_->mutex);
  if (flight_->done)
    return false;
  flight_->waiters.emplace_back(handle, loop);
  return true;
}

DriverResult StillFlights::Wait::await_resume() const
{
  const std::scoped_lock lock(flight_->mutex);
  return flight_->result;
}

StillFlights::Ticket::Ticket(StillFlights* leader, std::shared_ptr<Flight> flight)
    : leader_(leader), flight_(std::move(flight))
{
}

StillFlights::Ticket::~Ticket()
{
  if (leader_ != nullptr)
    leader_->finish(*flight_, DriverResult::failed("The camera sent no picture"));
}

bool StillFlights::Ticket::leads() const
{
  return leader_ != nullptr;
}

StillFlights::Wait StillFlights::Ticket::result() const
{
  return Wait(flight_);
}

void StillFlights::Ticket::land(const DriverResult& result)
{
  if (StillFlights* leader = std::exchange(leader_, nullptr); leader != nullptr)
    leader->finish(*flight_, result);
}

StillFlights::Ticket StillFlights::join(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  if (const auto found = flights_.find(cameraId); found != flights_.end())
    return {nullptr, found->second};
  auto flight = std::make_shared<Flight>();
  flight->cameraId = cameraId;
  flights_.emplace(cameraId, flight);
  return {this, std::move(flight)};
}

void StillFlights::finish(Flight& flight, const DriverResult& result)
{
  {
    const std::scoped_lock lock(mutex_);
    if (const auto found = flights_.find(flight.cameraId);
        found != flights_.end() && found->second.get() == &flight)
      flights_.erase(found);
  }
  std::vector<std::pair<std::coroutine_handle<>, trantor::EventLoop*>> waiters;
  {
    const std::scoped_lock lock(flight.mutex);
    flight.done = true;
    flight.result = result;
    waiters.swap(flight.waiters);
  }
  for (const auto& [handle, loop] : waiters)
    blocking_task::resumeOn(loop, handle);
}
