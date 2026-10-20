#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/agenda/repositories/agenda-announcement/agenda-announcement-repository.hxx>
#include <json/value.h>
#include <shared/repositories/reminder/reminder-repository.hxx>
#include <shared/schemas/reminder/reminder-schema.hxx>
#include <shared/services/reminder-series/reminder-series.hxx>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct AgendaNotice
{
  std::vector<int64_t> userIds;
  std::string title;
  std::string body;
  Json::Value data;
  std::string commandId;
  int leadMinutes{0};
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
  int64_t graceS{120};
  int64_t retentionS{2592000};
};

struct AgendaAnnouncerDependencies
{
  std::shared_ptr<const AgendaNotifier> notifier;
  std::function<int64_t()> clock;
  bool blockingOffLoop{true};
  std::function<bool()> eventsActive{};
};

struct AgendaSweepReport
{
  int64_t events{0};
  int64_t reminders{0};
  int64_t failed{0};
};

struct DueOccurrencesInput
{
  const std::vector<SeriesEventRow>& series;
  const std::set<EventNoticeKey>& notices;
  int64_t after{0};
  int64_t at{0};
};

namespace agenda_notice
{
std::string clockTime(int64_t epochSeconds);

std::vector<DueEventRow> dueOccurrences(const DueOccurrencesInput& input);

void orderDue(std::vector<DueEventRow>& events);

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
  drogon::Task<std::vector<DueEventRow>> dueEvents(int64_t at) const;
  drogon::Task<void> settleDueReminder(const DueReminderRow& reminder, int64_t at) const;
  drogon::Task<void> skipMissedSeries(const ReminderSchema& series, int64_t at) const;

  [[nodiscard]] int64_t now() const;

  AgendaAnnouncerConfig config_;
  AgendaAnnouncerDependencies dependencies_;
  AgendaAnnouncementRepository repository_;
  ReminderRepository reminders_;
  ReminderSeries series_;
};
