#include "action-journal.hxx"

#include <optional>
#include <sync/action-journal-fields.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-change-sink.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<bool> ActionJournal::shared(const ActionJournalInput& input) const
{
  if (input.title.empty() || input.sharingParentId <= 0)
    co_return false;
  const auto ceiling = action_journal::ceilingOf(input.table);
  if (!ceiling || !ceiling->title)
    co_return false;
  if (input.table == TableName::CalendarEvent)
    co_return !(co_await shareRepository_.memberIds(input.sharingParentId, input.client)).empty();
  co_return !(co_await memberRepository_.memberIds(input.sharingParentId, input.client)).empty();
}

drogon::Task<void> ActionJournal::record(const ActionJournalInput& input) const
{
  if (input.actorId <= 0)
    co_return;
  const auto* sink = user_change::getProductivitySink();
  if (sink == nullptr) {
    LOG_WARN << "user change sink not installed; drop "
             << tableNameToString(input.table) << " action";
    co_return;
  }
  const bool sharedRow = co_await shared(input);
  co_await sink->publishAction(
      {.event = {.userId = input.actorId,
                 .recordId = input.recordId,
                 .tableName = input.table,
                 .action = input.action,
                 .oldData = Json::Value(),
                 .newData = action_journal::journalData(
                     input.table, sharedRow ? std::optional<std::string>(input.title)
                                            : std::nullopt),
                 .ipAddress = "",
                 .module = "",
                 .subject = ""},
       .client = input.client});
}
