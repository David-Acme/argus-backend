#include "guard-alert-sink.hxx"

#include <feature/guard/guard-service.hxx>

GuardAlertSink::GuardAlertSink(GuardService& guard) : guard_(guard) {}

drogon::Task<bool> GuardAlertSink::raise(const SafetyAlertNotice& notice) const
{
  co_return co_await guard_.raiseSafetyAlert({.duress = notice.kind == SafetyAlertKind::Duress,
                                              .alertId = notice.alertId,
                                              .actorUserId = notice.actorUserId,
                                              .actorName = notice.actorName,
                                              .environmentId = notice.environmentId,
                                              .now = notice.now});
}
