#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/agenda/repositories/agenda-announcement/agenda-announcement-repository.hxx>
#include <json/value.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AgendaNotice
{
  std::vector<int64_t> userIds;
  std::string title;
  std::string body;
  Json::Value data;
  std::string commandId;
};

class AgendaNotifier
{
public:
  virtual ~AgendaNotifier() = default;

  [[nodiscard]] virtual bool send(const AgendaNotice& notice) const = 0;
};

struct AgendaAnnouncerConfig
{
  bool enabled{true};
  int64_t leadS{600};
  int64_t graceS{120};
  int64_t retentionS{2592000};
};

struct AgendaAnnouncerDependencies
{
  std::shared_ptr<const AgendaNotifier> notifier;
  std::function<int64_t()> clock;
  bool blockingOffLoop{true};
};

struct AgendaSweepReport
{
  int64_t events{0};
  int64_t reminders{0};
  int64_t failed{0};
};

namespace agenda_notice
{
std::string clockTime(int64_t epochSeconds);

AgendaNotice forEvent(const DueEventRow& event,
                      const std::vector<int64_t>& sharedWith);

AgendaNotice forReminder(const DueReminderRow& reminder);
}

class AgendaAnnouncer
{
public:
  AgendaAnnouncer(AgendaAnnouncerConfig config,
                  AgendaAnnouncerDependencies dependencies);

  drogon::Task<AgendaSweepReport> sweep() const;

  [[nodiscard]] bool enabled() const;

private:
  drogon::Task<bool> deliver(const AgendaNotice& notice) const;

  [[nodiscard]] int64_t now() const;

  AgendaAnnouncerConfig config_;
  AgendaAnnouncerDependencies dependencies_;
  AgendaAnnouncementRepository repository_;
};
