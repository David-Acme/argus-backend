#include "voiceprint-audit.hxx"

#include <sync/identity-change-sink.hxx>

namespace voiceprint_audit
{

drogon::Task<void> publish(const VoiceprintAuditInput& input)
{
  const auto* sink = identity_change::getSink();
  if (sink == nullptr)
    co_return;
  const ActionPublishInput publish{.event = {.userId = input.actorId,
                                             .recordId = input.subjectId,
                                             .tableName = TableName::User,
                                             .action = input.action,
                                             .oldData = Json::Value(),
                                             .newData = input.data,
                                             .ipAddress = ""},
                                   .client = input.client};
  co_await sink->publishAction(publish);
}

}
