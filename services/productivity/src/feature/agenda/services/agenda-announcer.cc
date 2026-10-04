#include "agenda-announcer.hxx"

#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <utility>

namespace
{
constexpr int kBatch = 100;

void addUser(std::vector<int64_t>& users, int64_t userId)
{
  if (userId > 0 && std::ranges::find(users, userId) == users.end())
    users.push_back(userId);
}
}

std::string agenda_notice::clockTime(int64_t epochSeconds)
{
  const auto seconds = static_cast<std::time_t>(epochSeconds);
  std::tm local{};
  localtime_r(&seconds, &local);
  std::array<char, 8> buffer{};
  const std::size_t written =
      std::strftime(buffer.data(), buffer.size(), "%H:%M", &local);
  return {buffer.data(), written};
}

AgendaNotice agenda_notice::forEvent(const DueEventRow& event,
                                     const std::vector<int64_t>& sharedWith)
{
  AgendaNotice notice;
  addUser(notice.userIds, event.ownerId);
  for (const int64_t userId : sharedWith)
    addUser(notice.userIds, userId);
  const std::string threadKey = "agenda:event:" + std::to_string(event.id) +
                                ":" + std::to_string(event.startsAt);
  notice.title = event.title;
  notice.body = clockTime(event.startsAt);
  if (!event.location.empty())
    notice.body += " · " + event.location;
  notice.data = Json::Value(Json::objectValue);
  notice.data["kind"] = "agenda_event";
  notice.data["eventId"] = static_cast<Json::Int64>(event.id);
  notice.data["title"] = event.title;
  notice.data["startsAt"] = static_cast<Json::Int64>(event.startsAt);
  notice.data["location"] = event.location;
  notice.data["threadKey"] = threadKey;
  notice.data["urgency"] = "time_sensitive";
  notice.commandId = threadKey;
  return notice;
}

AgendaNotice agenda_notice::forReminder(const DueReminderRow& reminder)
{
  AgendaNotice notice;
  addUser(notice.userIds, reminder.targetUserId);
  const std::string threadKey = "agenda:reminder:" +
                                std::to_string(reminder.id) + ":" +
                                std::to_string(reminder.scheduledAt);
  notice.title = reminder.title;
  notice.body = reminder.description;
  notice.data = Json::Value(Json::objectValue);
  notice.data["kind"] = "agenda_reminder";
  notice.data["reminderId"] = static_cast<Json::Int64>(reminder.id);
  notice.data["title"] = reminder.title;
  notice.data["scheduledAt"] = static_cast<Json::Int64>(reminder.scheduledAt);
  notice.data["threadKey"] = threadKey;
  notice.data["urgency"] = "time_sensitive";
  notice.commandId = threadKey;
  return notice;
}

AgendaAnnouncer::AgendaAnnouncer(AgendaAnnouncerConfig config,
                                 AgendaAnnouncerDependencies dependencies)
    : config_(config), dependencies_(std::move(dependencies))
{
}

bool AgendaAnnouncer::enabled() const
{
  return config_.enabled && dependencies_.notifier != nullptr;
}

int64_t AgendaAnnouncer::now() const
{
  if (dependencies_.clock)
    return dependencies_.clock();
  return static_cast<int64_t>(std::time(nullptr));
}

drogon::Task<bool> AgendaAnnouncer::deliver(const AgendaNotice& notice) const
{
  if (notice.userIds.empty())
    co_return true;
  if (!dependencies_.blockingOffLoop)
    co_return dependencies_.notifier->send(notice);
  const auto notifier = dependencies_.notifier;
  co_return co_await BlockingTask<bool>(
      [notifier, notice]() { return notifier->send(notice); });
}

drogon::Task<AgendaSweepReport> AgendaAnnouncer::sweep() const
{
  AgendaSweepReport report;
  if (!enabled())
    co_return report;
  const int64_t at = now();

  const auto events = co_await repository_.dueEvents(
      {.after = at - config_.graceS, .until = at + config_.leadS, .limit = kBatch});
  std::vector<int64_t> eventIds;
  eventIds.reserve(events.size());
  for (const auto& event : events)
    eventIds.push_back(event.id);
  const auto shares = co_await repository_.sharesOf(eventIds);
  for (const auto& event : events) {
    const auto found = shares.find(event.id);
    const AgendaNotice notice = agenda_notice::forEvent(
        event, found == shares.end() ? std::vector<int64_t>{} : found->second);
    if (!co_await deliver(notice)) {
      ++report.failed;
      continue;
    }
    co_await repository_.record({.kind = "event",
                                 .refId = event.id,
                                 .occurrenceAt = event.startsAt,
                                 .at = at});
    ++report.events;
  }

  const auto reminders = co_await repository_.dueReminders(
      {.after = at - config_.graceS, .until = at, .limit = kBatch});
  for (const auto& reminder : reminders) {
    if (!co_await deliver(agenda_notice::forReminder(reminder))) {
      ++report.failed;
      continue;
    }
    co_await repository_.record({.kind = "reminder",
                                 .refId = reminder.id,
                                 .occurrenceAt = reminder.scheduledAt,
                                 .at = at});
    ++report.reminders;
  }

  co_await repository_.purgeBefore(at - config_.retentionS);
  if (report.failed > 0)
    LOG_WARN << "Agenda: " << report.failed
             << " announcement(s) not delivered; retried next sweep";
  co_return report;
}
