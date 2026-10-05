#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/agenda/services/agenda-announcer.hxx>

#include <atomic>
#include <memory>

class AgendaSweeper
{
public:
  explicit AgendaSweeper(std::shared_ptr<const AgendaAnnouncer> announcer);

  void start(double periodS);

  [[nodiscard]] drogon::Task<bool> tick();

  void requestStop();

  [[nodiscard]] bool drained() const;

private:
  std::shared_ptr<const AgendaAnnouncer> announcer_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
};
