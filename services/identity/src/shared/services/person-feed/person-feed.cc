#include "person-feed.hxx"

#include <sync/identity-change-sink.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>

drogon::Task<void> PersonFeed::added(const PersonFeedAddInput& input) const
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr || !input.person.isHousehold())
    co_return;
  SocketEmitDto frame;
  frame.operation = SyncOperation::Add;
  frame.option = TableName::Person;
  frame.obj = input.person.toHouseholdJson();
  co_await sink->emitModule(
      {.table = TableName::Person, .body = frame, .client = input.client});
}

drogon::Task<void> PersonFeed::changed(const PersonFeedChangeInput& input) const
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr || !input.before.isHousehold() || !input.after.isHousehold())
    co_return;
  co_await sink->publishModuleAudit({.recordId = input.after.id,
                                     .tableName = TableName::Person,
                                     .before = input.before.toHouseholdJson(),
                                     .after = input.after.toHouseholdJson(),
                                     .actorId = input.actorId,
                                     .client = input.client});
}
