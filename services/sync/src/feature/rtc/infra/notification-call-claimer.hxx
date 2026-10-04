#pragma once

#include <feature/rtc/infra/rtc-ports.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationCallClaimer final : public RtcCallClaimer
{
public:
  explicit NotificationCallClaimer(std::shared_ptr<const NotificationClient> client);
  drogon::Task<RtcClaim> claim(RtcClaimInput input) const override;

  [[nodiscard]] static RtcClaim claimOf(const NotificationCallClaimResult& result);

private:
  std::shared_ptr<const NotificationClient> client_;
};
