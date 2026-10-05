#include "agenda-sweeper.hxx"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <exception>
#include <utility>

AgendaSweeper::AgendaSweeper(std::shared_ptr<const AgendaAnnouncer> announcer)
    : announcer_(std::move(announcer))
{
}

void AgendaSweeper::start(double periodS)
{
  drogon::app().getLoop()->runEvery(periodS, [this]() {
    drogon::async_run([this]() -> drogon::Task<void> { co_await tick(); });
  });
}

drogon::Task<bool> AgendaSweeper::tick()
{
  if (stopping_.load(std::memory_order_acquire) ||
      running_.exchange(true, std::memory_order_acq_rel))
    co_return false;
  if (stopping_.load(std::memory_order_acquire)) {
    running_.store(false, std::memory_order_release);
    co_return false;
  }
  try {
    co_await announcer_->sweep();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Agenda: sweep failed: " << error.what();
  }
  running_.store(false, std::memory_order_release);
  co_return true;
}

void AgendaSweeper::requestStop()
{
  stopping_.store(true, std::memory_order_release);
}

bool AgendaSweeper::drained() const
{
  return !running_.load(std::memory_order_acquire);
}
