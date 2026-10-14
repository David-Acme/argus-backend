#pragma once

#include <feature/rtc/infra/rtc-ports.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationCallDecliner final : public RtcCallDecliner
{
public:
  explicit NotificationCallDecliner(std::shared_ptr<const NotificationClient> client);
  drogon::Task<RtcDecline> decline(RtcDeclineInput input) const override;

  [[nodiscard]] static RtcDecline declineOf(const NotificationCallEndResult& result);

private:
  std::shared_ptr<const NotificationClient> client_;
};
