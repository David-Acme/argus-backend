#include "agenda-announcement-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

#include <string>

using namespace agenda_announcement_query;

drogon::Task<std::vector<DueEventRow>>
AgendaAnnouncementRepository::dueEvents(const AgendaWindowInput& input) const
{
  const auto client = DbService::productivityClient();
  const auto rows = co_await client->execSqlCoro(
      std::string(DUE_EVENTS), input.until, input.after,
      input.until + kMaxLeadS, input.limit);
  std::vector<DueEventRow> events;
  events.reserve(rows.size());
  for (const auto& row : rows)
    events.push_back({.id = row["id"].as<int64_t>(),
                      .ownerId = row["owner_id"].as<int64_t>(),
                      .title = row["title"].as<std::string>(),
                      .location = row["location"].as<std::string>(),
                      .startsAt = row["starts_at"].as<int64_t>(),
                      .leadMinutes = row["minutes"].as<int>()});
  co_return events;
}

drogon::Task<std::vector<SeriesEventRow>>
AgendaAnnouncementRepository::liveSeries(int64_t until) const
{
  const auto client = DbService::productivityClient();
  const auto rows = co_await client->execSqlCoro(std::string(LIVE_SERIES), until);
  std::vector<SeriesEventRow> series;
  series.reserve(rows.size());
  for (const auto& row : rows)
    series.push_back({.id = row["id"].as<int64_t>(),
                      .ownerId = row["owner_id"].as<int64_t>(),
                      .title = row["title"].as<std::string>(),
                      .location = row["location"].as<std::string>(),
                      .startsAt = row["starts_at"].as<int64_t>(),
                      .recurrenceRule = row["recurrence_rule"].as<std::string>(),
                      .recurrenceExceptions = row["recurrence_exceptions"].as<std::string>()});
  co_return series;
}

drogon::Task<std::set<EventNoticeKey>>
AgendaAnnouncementRepository::eventNotices(const AgendaNoticesInput& input) const
{
  std::set<EventNoticeKey> notices;
  if (input.eventIds.empty())
    co_return notices;
  std::string sql(EVENT_NOTICES_PREFIX);
  std::vector<std::string> args;
  args.reserve(input.eventIds.size() + 2);
  args.push_back(std::to_string(input.after));
  args.push_back(std::to_string(input.until));
  for (std::size_t index = 0; index < input.eventIds.size(); ++index) {
    sql += index == 0 ? "?" : ", ?";
    args.push_back(std::to_string(input.eventIds[index]));
  }
  sql += ')';
  const auto client = DbService::productivityClient();
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(sql, argsRef);
  for (const auto& row : rows)
    notices.insert(EventNoticeKey{.refId = row["ref_id"].as<int64_t>(),
                    .occurrenceAt = row["occurrence_at"].as<int64_t>(),
                    .leadMinutes = row["lead_minutes"].as<int>()});
  co_return notices;
}

drogon::Task<std::map<int64_t, std::vector<int64_t>>>
AgendaAnnouncementRepository::sharesOf(const std::vector<int64_t>& eventIds) const
{
  std::map<int64_t, std::vector<int64_t>> shares;
  if (eventIds.empty())
    co_return shares;
  std::string sql(SHARES_PREFIX);
  std::vector<std::string> args;
  args.reserve(eventIds.size());
  for (std::size_t index = 0; index < eventIds.size(); ++index) {
    sql += index == 0 ? "?" : ", ?";
    args.push_back(std::to_string(eventIds[index]));
  }
  sql += ')';
  const auto client = DbService::productivityClient();
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(sql, argsRef);
  for (const auto& row : rows)
    shares[row["calendar_event_id"].as<int64_t>()].push_back(
        row["user_id"].as<int64_t>());
  co_return shares;
}

drogon::Task<std::vector<DueReminderRow>>
AgendaAnnouncementRepository::dueReminders(const AgendaWindowInput& input) const
{
  const auto client = DbService::productivityClient();
  const auto rows = co_await client->execSqlCoro(
      std::string(DUE_REMINDERS), input.after, input.until, input.limit);
  std::vector<DueReminderRow> reminders;
  reminders.reserve(rows.size());
  for (const auto& row : rows)
    reminders.push_back({.id = row["id"].as<int64_t>(),
                         .targetUserId = row["target_user_id"].as<int64_t>(),
                         .title = row["title"].as<std::string>(),
                         .description = row["description"].as<std::string>(),
                         .scheduledAt = row["scheduled_at"].as<int64_t>()});
  co_return reminders;
}

drogon::Task<bool>
AgendaAnnouncementRepository::record(const AgendaRecordInput& input) const
{
  const auto pooled = input.client ? nullptr : DbService::productivityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      std::string(RECORD), input.kind, input.refId, input.occurrenceAt,
      input.leadMinutes, input.at);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
AgendaAnnouncementRepository::purgeBefore(int64_t occurrenceAt) const
{
  const auto client = DbService::productivityClient();
  const auto result = co_await client->execSqlCoro(std::string(PURGE), occurrenceAt);
  co_return static_cast<int64_t>(result.affectedRows());
}
