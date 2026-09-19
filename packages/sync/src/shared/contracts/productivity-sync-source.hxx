#pragma once

#include <filter/jwt/jwt-filter.hxx>
#include <memory>
#include <shared/contracts/syncable.hxx>

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

// Pull source for the productivity-domain sync tables.
class ProductivitySyncSource
{
public:
  virtual ~ProductivitySyncSource() = default;

  virtual bool serves(ProductivitySyncTable table) const = 0;

  virtual std::unique_ptr<Syncable>
  sourceFor(ProductivitySyncTable table, const JwtContext& ctx) const = 0;
};
