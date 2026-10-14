#include "notification-call-decliner.hxx"

#include <runtime/blocking-task.hxx>

#include <utility>

NotificationCallDecliner::NotificationCallDecliner(std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

RtcDecline NotificationCallDecliner::declineOf(const NotificationCallEndResult& result)
{
  RtcDecline decline;
  if (result.outcome != NotificationRpcOutcome::Success && result.outcome != NotificationRpcOutcome::Rejected)
    return decline;
  switch (result.status) {
    case argus::notification::v1::CALL_END_STATUS_ENDED:
      if (result.outcome == NotificationRpcOutcome::Success)
        decline.status = RtcDeclineStatus::Declined;
      break;
    case argus::notification::v1::CALL_END_STATUS_TAKEN:
      decline.status = RtcDeclineStatus::Taken;
      break;
    case argus::notification::v1::CALL_END_STATUS_EXPIRED:
      decline.status = RtcDeclineStatus::Expired;
      break;
    case argus::notification::v1::CALL_END_STATUS_NOT_FOUND:
      decline.status = RtcDeclineStatus::NotFound;
      break;
    default:
      break;
  }
  return decline;
}

drogon::Task<RtcDecline> NotificationCallDecliner::decline(RtcDeclineInput input) const
{
  const NotificationCallEndResult result = co_await BlockingTask<NotificationCallEndResult>{
      [client = client_, request = std::move(input)] {
        return client->endCallWithStatus({.callId = request.callId,
                                          .userId = request.userId,
                                          .outcome = argus::notification::v1::CALL_OUTCOME_DECLINED,
                                          .spoken = false,
                                          .sessionId = request.sessionId});
      }};
  co_return declineOf(result);
}
