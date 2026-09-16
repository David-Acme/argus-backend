#pragma once
#include "notification-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/notification/notification-schema.hxx>

struct NotificationReadChange
{
  NotificationSchema before;
  NotificationSchema after;
};

class NotificationRepository
{
public:
  NotificationRepository() = default;

  drogon::Task<NotificationSchema>
  create(const NotificationCreateInput& input) const;
  drogon::Task<std::vector<NotificationSchema>>
  createMany(const std::vector<NotificationCreateInput>& inputs) const;

  // One IMMEDIATE transaction: command claim, the whole batch and one durable
  // delivery intent per row. Duplicate returns the persisted expected count;
  // a real failure throws.
  drogon::Task<NotificationCommitResult> createManyWithCommand(
      const NotificationBatchCommitInput& input) const;

  drogon::Task<std::vector<NotificationDeliveryRow>> pendingDeliveries(
      int limit) const;

  drogon::Task<int64_t> pendingDeliveryCount() const;

  drogon::Task<bool> markDelivered(int64_t deliveryId, int64_t at) const;

  drogon::Task<int64_t> ackDeliveries(const AckDeliveriesInput& input) const;

  drogon::Task<DeliverySummary> deliverySummary(
      const DeliverySummaryInput& input) const;

  drogon::Task<ProbeInsertResult> insertProbe(int64_t at, int64_t atMs) const;

  drogon::Task<std::string> deliveryState(int64_t deliveryId) const;

  drogon::Task<SelfTestState> probeState() const;

  drogon::Task<bool> recordProbe(const ProbeRecordInput& input) const;

  drogon::Task<int64_t> purgeProbes(int64_t olderThan) const;

  drogon::Task<std::vector<Json::Value>>
  findSync(const NotificationSyncFilter& filter) const;
  drogon::Task<std::optional<Json::Value>>
  findLastSync(const NotificationSyncFilter& filter) const;

  drogon::Task<std::vector<NotificationReadChange>>
  markAsRead(int64_t userId, const std::vector<int64_t>& ids) const;
};
