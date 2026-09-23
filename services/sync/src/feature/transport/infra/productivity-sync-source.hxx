#pragma once

#include <auth/jwt-filter.hxx>
#include <memory>
#include <sync/syncable.hxx>

enum class ProductivitySyncTable
{
  Reminder,
  ReminderDetail,
  CalendarEvent,
  CalendarEventShare,
  Project,
  ProjectMember,
  ProjectTask,
};

class ProductivitySyncSource
{
public:
  virtual ~ProductivitySyncSource() = default;

  [[nodiscard]] virtual bool serves(ProductivitySyncTable table) const = 0;

  [[nodiscard]] virtual std::unique_ptr<Syncable>
  sourceFor(ProductivitySyncTable table, const JwtContext& ctx) const = 0;
};
