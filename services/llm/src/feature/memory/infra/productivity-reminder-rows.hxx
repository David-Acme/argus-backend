#pragma once

#include <feature/memory/services/memory/reminder-row-writer.hxx>
#include <productivity/productivity-reminder-client.hxx>

#include <memory>

class ProductivityReminderRows final : public ReminderRowWriter
{
public:
  explicit ProductivityReminderRows(std::shared_ptr<const ProductivityReminderClient> client);

  [[nodiscard]] bool create(const ReminderRowRequest& request) const override;

  [[nodiscard]] std::optional<std::vector<ReminderRowInfo>> list(const ReminderRowQuery& query) const override;

private:
  std::shared_ptr<const ProductivityReminderClient> client_;
};
