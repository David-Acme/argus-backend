#include "productivity-reminder-rows.hxx"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <utility>

namespace
{
constexpr const char* kDevice = "argus-llm";
constexpr size_t kKeyLength = 64;

argus::client::CallerIdentity identityOf(int64_t userId, const std::string& role)
{
  return {.userId = userId, .role = role, .device = std::string(kDevice)};
}

std::string keyOf(const std::string& commandId)
{
  std::string key = commandId;
  std::ranges::replace_if(
      key, [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'); },
      '-');
  if (key.size() > kKeyLength)
    key.resize(kKeyLength);
  return key;
}
}

ProductivityReminderRows::ProductivityReminderRows(std::shared_ptr<const ProductivityReminderClient> client)
    : client_(std::move(client))
{
}

bool ProductivityReminderRows::create(const ReminderRowRequest& request) const
{
  if (!client_)
    return false;
  const auto result = client_->create({.identity = identityOf(request.userId, request.role),
                                       .title = request.title,
                                       .description = {},
                                       .scheduledAt = request.scheduledAt,
                                       .recurrenceRule = std::nullopt,
                                       .idempotencyKey = keyOf(request.commandId)});
  if (result.outcome == ReminderRpcOutcome::Success)
    return true;
  LOG_WARN << "argus-llm: reminder row " << request.commandId << " not created: " << result.message;
  return false;
}

std::optional<std::vector<ReminderRowInfo>> ProductivityReminderRows::list(const ReminderRowQuery& query) const
{
  if (!client_)
    return std::nullopt;
  const auto result = client_->list({.identity = identityOf(query.userId, query.role),
                                     .includeCompleted = query.includeCompleted,
                                     .limit = query.limit});
  if (result.outcome != ReminderRpcOutcome::Success) {
    LOG_WARN << "argus-llm: reminders not listed: " << result.message;
    return std::nullopt;
  }
  std::vector<ReminderRowInfo> rows;
  rows.reserve(result.reminders.size());
  for (const auto& row : result.reminders)
    rows.push_back({.id = row.id(), .title = row.title(), .scheduledAt = row.scheduled_at(), .completed = row.is_completed()});
  return rows;
}
