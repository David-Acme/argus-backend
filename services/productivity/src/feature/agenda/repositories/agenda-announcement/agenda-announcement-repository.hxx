#pragma once

#include "agenda-announcement-query.hxx"

#include <drogon/utils/coroutine.h>

#include <map>
#include <vector>

struct DueEventRow
{
  int64_t id{0};
  int64_t ownerId{0};
  std::string title;
  std::string location;
  int64_t startsAt{0};
};

struct DueReminderRow
{
  int64_t id{0};
  int64_t targetUserId{0};
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
};

class AgendaAnnouncementRepository
{
public:
  AgendaAnnouncementRepository() = default;

  drogon::Task<std::vector<DueEventRow>>
  dueEvents(const AgendaWindowInput& input) const;

  drogon::Task<std::map<int64_t, std::vector<int64_t>>>
  sharesOf(const std::vector<int64_t>& eventIds) const;

  drogon::Task<std::vector<DueReminderRow>>
  dueReminders(const AgendaWindowInput& input) const;

  drogon::Task<bool> record(const AgendaRecordInput& input) const;

  drogon::Task<int64_t> purgeBefore(int64_t occurrenceAt) const;
};
