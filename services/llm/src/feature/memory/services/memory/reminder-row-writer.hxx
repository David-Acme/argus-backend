#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ReminderRowRequest
{
  int64_t userId{0};
  std::string role;
  std::string title;
  int64_t scheduledAt{0};
  std::string commandId;
};

struct ReminderRowQuery
{
  int64_t userId{0};
  std::string role;
  bool includeCompleted{false};
  int limit{0};
};

struct ReminderRowInfo
{
  int64_t id{0};
  std::string title;
  int64_t scheduledAt{0};
  bool completed{false};
};

class ReminderRowWriter
{
public:
  ReminderRowWriter() = default;
  virtual ~ReminderRowWriter() = default;
  ReminderRowWriter(const ReminderRowWriter&) = delete;
  ReminderRowWriter& operator=(const ReminderRowWriter&) = delete;

  [[nodiscard]] virtual bool create(const ReminderRowRequest& request) const = 0;

  [[nodiscard]] virtual std::optional<std::vector<ReminderRowInfo>> list(const ReminderRowQuery& query) const = 0;
};
