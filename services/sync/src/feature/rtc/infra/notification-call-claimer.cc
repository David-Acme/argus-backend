#include "notification-call-claimer.hxx"

#include <runtime/blocking-task.hxx>

#include <utility>

NotificationCallClaimer::NotificationCallClaimer(std::shared_ptr<const NotificationClient> client)
    : client_(std::move(client))
{
}

RtcClaim NotificationCallClaimer::claimOf(const NotificationCallClaimResult& result)
{
  RtcClaim claim;
  if (result.outcome != NotificationRpcOutcome::Success)
    return claim;
  const auto& response = result.response;
  switch (response.status()) {
    case argus::notification::v1::CALL_CLAIM_STATUS_CLAIMED:
      claim.status = RtcClaimStatus::Claimed;
      break;
    case argus::notification::v1::CALL_CLAIM_STATUS_TAKEN:
      claim.status = RtcClaimStatus::Taken;
      return claim;
    case argus::notification::v1::CALL_CLAIM_STATUS_EXPIRED:
      claim.status = RtcClaimStatus::Expired;
      return claim;
    case argus::notification::v1::CALL_CLAIM_STATUS_NOT_FOUND:
      claim.status = RtcClaimStatus::NotFound;
      return claim;
    default:
      return claim;
  }
  claim.openingLine = response.opening_line();
  claim.lang = response.lang();
  claim.kind = response.kind();
  claim.summary = response.summary();
  claim.cameraId = response.camera_id();
  claim.cameraName = response.camera_name();
  claim.episodeId = response.episode_id();
  return claim;
}

drogon::Task<RtcClaim> NotificationCallClaimer::claim(RtcClaimInput input) const
{
  const NotificationCallClaimResult result = co_await BlockingTask<NotificationCallClaimResult>{
      [client = client_, request = std::move(input)] {
        return client->claimCall(
            {.callId = request.callId, .userId = request.userId, .sessionId = request.sessionId});
      }};
  co_return claimOf(result);
}
