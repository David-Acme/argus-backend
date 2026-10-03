#include "notification-feature-service.hxx"

#include <ctime>
#include <config/notification-config.hxx>

namespace
{
constexpr int64_t kDefaultSummaryWindowS = 24LL * 3600;
}

drogon::Task<void>
NotificationFeatureService::markAsRead(int64_t userId,
                                       const std::vector<int64_t>& ids) const
{
  co_await notificationService_.markAsRead(userId, ids);
  co_return;
}

drogon::Task<int64_t> NotificationFeatureService::ackDeliveries(
    int64_t userId, const std::vector<int64_t>& notificationIds) const
{
  co_return co_await notificationService_.ackDeliveries(userId,
                                                        notificationIds);
}

drogon::Task<Json::Value> NotificationFeatureService::deliverySummary(
    int64_t since) const
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const DeliverySummary summary = co_await notificationService_.deliverySummary(
      since > 0 ? since : now - kDefaultSummaryWindowS, NotificationConfig::resolveAckWindowS());
  Json::Value response(Json::objectValue);
  response["pending"] = Json::Int64(summary.pending);
  response["unacked"] = Json::Int64(summary.unacked);
  response["unackedOld"] = Json::Int64(summary.unackedOld);
  response["sent"] = Json::Int64(summary.sent);
  response["acked"] = Json::Int64(summary.acked);
  response["latencyMsP50"] = Json::Int64(summary.latencyMsP50);
  response["latencyMsP95"] = Json::Int64(summary.latencyMsP95);
  response["latencyMsMax"] = Json::Int64(summary.latencyMsMax);
  response["probeAt"] = Json::Int64(summary.probeAt);
  response["probeOk"] = summary.probeOk;
  response["probeMs"] = Json::Int64(summary.probeMs);
  co_return response;
}
