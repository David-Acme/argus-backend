#include "guard-alert-sink.hxx"

#include <feature/guard/guard-service.hxx>

GuardAlertSink::GuardAlertSink(GuardService& guard) : guard_(guard) {}

drogon::Task<SafetyDelivery> GuardAlertSink::raise(const SafetyAlertNotice& notice) const
{
  const GuardService::SafetyAlertOutcome outcome =
      co_await guard_.raiseSafetyAlert({.duress = notice.kind == SafetyAlertKind::Duress,
                                        .alertId = notice.alertId,
                                        .actorUserId = notice.actorUserId,
                                        .actorName = notice.actorName,
                                        .environmentId = notice.environmentId,
                                        .now = notice.now,
                                        .sequence = notice.sequence});
  if (outcome.accepted)
    co_return SafetyDelivery::Sent;
  if (outcome.unattended)
    co_return SafetyDelivery::NoRecipients;
  co_return outcome.terminal ? SafetyDelivery::Refused : SafetyDelivery::Pending;
}
