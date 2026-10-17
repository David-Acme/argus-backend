#include "notification-feature-service.hxx"

#include <ctime>
#include <config/notification-config.hxx>

namespace
{
constexpr int64_t kDefaultSummaryWindowS = 24LL * 3600;
}

NotificationFeatureService::NotificationFeatureService(
    std::shared_ptr<const NotificationService> notifications)
    : notificationService_(std::move(notifications))
{
}

drogon::Task<void>
NotificationFeatureService::markAsRead(int64_t userId,
                                       const std::vector<int64_t>& ids) const
{
  co_await notificationService_->markAsRead(userId, ids);
  co_return;
}

drogon::Task<void> NotificationFeatureService::markThreadAsRead(
    const NotificationThreadRef& ref) const
{
  co_await notificationService_->markThreadAsRead(ref);
  co_return;
}

drogon::Task<bool>
NotificationFeatureService::remove(const NotificationRef& ref) const
{
  co_return co_await notificationService_->remove(ref);
}

drogon::Task<bool>
NotificationFeatureService::removeThread(const NotificationThreadRef& ref) const
{
  co_return co_await notificationService_->removeThread(ref);
}

drogon::Task<int64_t> NotificationFeatureService::ackDeliveries(
    int64_t userId, const std::vector<int64_t>& notificationIds) const
{
  co_return co_await notificationService_->ackDeliveries(userId,
                                                        notificationIds);
}

drogon::Task<Json::Value> NotificationFeatureService::deliverySummary(
    int64_t since) const
{
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const DeliverySummary summary = co_await notificationService_->deliverySummary(
      since > 0 ? since : now - kDefaultSummaryWindowS, NotificationConfig::resolveAckWindowS());
  Json::Value response(Json::objectValue);
  response["pending"] = summary.pending;
  response["unacked"] = summary.unacked;
  response["unackedOld"] = summary.unackedOld;
  response["sent"] = summary.sent;
  response["acked"] = summary.acked;
  response["latencyMsP50"] = summary.latencyMsP50;
  response["latencyMsP95"] = summary.latencyMsP95;
  response["latencyMsMax"] = summary.latencyMsMax;
  response["probeAt"] = summary.probeAt;
  response["probeOk"] = summary.probeOk;
  response["probeMs"] = summary.probeMs;
  co_return response;
}
