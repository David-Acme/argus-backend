#include "notification-repository.hxx"

#include <algorithm>
#include <chrono>
#include <config/app-config.hxx>
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/utils/sha256/sha256.hxx>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace notification_query;

namespace
{
int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
// Commits in the transaction destructor; the callback resumes the awaiter.
class TransactionCommitAwaiter
{
public:
  explicit TransactionCommitAwaiter(
      std::shared_ptr<drogon::orm::Transaction> transaction)
      : transaction_(std::move(transaction))
  {
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) noexcept
  {
    auto transaction = std::move(transaction_);
    if (!transaction) {
      committed_ = false;
      handle.resume();
      return;
    }
    transaction->setCommitCallback([this, handle](bool committed) {
      committed_ = committed;
      handle.resume();
    });
    transaction.reset();
  }

  bool await_resume() const noexcept { return committed_; }

private:
  std::shared_ptr<drogon::orm::Transaction> transaction_;
  bool committed_{false};
};

std::string batchInsertSql(const std::vector<NotificationCreateInput>& inputs,
                           std::vector<std::string>& args)
{
  std::string sql{INSERT_MANY_PREFIX};
  args.reserve(inputs.size() * 5);
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (i > 0)
      sql += ", ";
    sql += "(?, ?, ?, ?, ?)";
    const auto& input = inputs[i];
    args.push_back(std::to_string(input.userId));
    args.push_back(input.type);
    args.push_back(input.title);
    args.push_back(input.body);
    args.push_back(json_util::toString(input.data));
  }
  sql += INSERT_MANY_SUFFIX;
  return sql;
}

// Length-prefixed so adjacent fields can never collide.
void addFingerprintField(argus::hash::Sha256& hasher, std::string_view value)
{
  hasher.update(std::to_string(value.size()));
  hasher.update(":");
  hasher.update(value);
  hasher.update("\n");
}

std::vector<NotificationCreateInput>
normalizeBatch(const std::vector<NotificationCreateInput>& inputs)
{
  if (inputs.empty())
    throw NotificationValidationError("recipient set is empty");
  if (inputs.size() > kMaxNotificationRecipients)
    throw NotificationValidationError("recipient set is too large");

  const NotificationCreateInput& first = inputs.front();
  if (first.type.size() > kMaxNotificationType ||
      first.title.size() > kMaxNotificationTitle ||
      first.body.size() > kMaxNotificationBody ||
      json_util::toString(first.data).size() > kMaxNotificationData)
    throw NotificationValidationError("notification field out of bounds");

  std::vector<int64_t> userIds;
  userIds.reserve(inputs.size());
  for (const auto& input : inputs) {
    if (input.userId <= 0)
      throw NotificationValidationError("recipient id must be positive");
    if (input.type != first.type || input.title != first.title ||
        input.body != first.body || input.data != first.data)
      throw NotificationValidationError("recipients must share one payload");
    userIds.push_back(input.userId);
  }
  std::sort(userIds.begin(), userIds.end());
  userIds.erase(std::unique(userIds.begin(), userIds.end()), userIds.end());

  std::vector<NotificationCreateInput> normalized;
  normalized.reserve(userIds.size());
  for (const int64_t userId : userIds) {
    NotificationCreateInput entry = first;
    entry.userId = userId;
    normalized.push_back(std::move(entry));
  }
  return normalized;
}

std::string
notificationFingerprint(const std::vector<NotificationCreateInput>& inputs)
{
  argus::hash::Sha256 hasher;
  addFingerprintField(hasher, "notification-v1");
  addFingerprintField(hasher, std::to_string(inputs.size()));
  for (const auto& input : inputs) {
    addFingerprintField(hasher, std::to_string(input.userId));
    addFingerprintField(hasher, input.type);
    addFingerprintField(hasher, input.title);
    addFingerprintField(hasher, input.body);
    addFingerprintField(hasher, json_util::toString(input.data));
  }
  return argus::hash::hex(hasher.digest());
}

std::vector<NotificationSchema>
schemasFromIds(const NotificationSchemaBuildInput& input)
{
  std::vector<NotificationSchema> schemas;
  schemas.reserve(input.inputs.size());
  for (std::size_t i = 0; i < input.inputs.size(); ++i) {
    NotificationSchema schema;
    schema.id = input.ids[i];
    schema.userId = input.inputs[i].userId;
    schema.type = input.inputs[i].type;
    schema.title = input.inputs[i].title;
    schema.body = input.inputs[i].body;
    schema.data = input.inputs[i].data;
    schema.createdAt = input.now;
    schemas.push_back(std::move(schema));
  }
  return schemas;
}
} // namespace

drogon::Task<NotificationSchema>
NotificationRepository::create(const NotificationCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.userId, input.type,
                                   input.title, input.body,
                                   json_util::toString(input.data));

  NotificationSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.type = input.type;
  schema.title = input.title;
  schema.body = input.body;
  schema.data = input.data;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::vector<NotificationSchema>>
NotificationRepository::createMany(
    const std::vector<NotificationCreateInput>& inputs) const
{
  if (inputs.empty())
    co_return {};
  std::vector<std::string> args;
  auto client = DbService::client();
  const auto& argsRef = args;
  const auto result =
      co_await client->execSqlCoro(batchInsertSql(inputs, args), argsRef);

  std::vector<int64_t> ids;
  ids.reserve(result.size());
  for (const auto& row : result)
    ids.push_back(row["id"].as<int64_t>());
  if (ids.size() != inputs.size())
    throw std::runtime_error("notification batch insert returned wrong size");
  std::sort(ids.begin(), ids.end());
  co_return schemasFromIds({.inputs = inputs,
                             .ids = ids,
                             .now = std::time(nullptr)});
}

drogon::Task<NotificationCommitResult>
NotificationRepository::createManyWithCommand(
    const NotificationBatchCommitInput& input) const
{
  NotificationCommitResult result;
  const std::vector<NotificationCreateInput> normalized =
      normalizeBatch(input.inputs);

  auto client = DbService::client();
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await client->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception&) {
    throw std::runtime_error("notification transaction unavailable");
  }

  const std::string fingerprint = notificationFingerprint(normalized);
  std::vector<int64_t> ids;
  std::optional<std::string> conflict;
  bool duplicate = false;
  int64_t duplicateCount = 0;
  try {
    if (!input.commandId.empty()) {
      const auto claimed =
          co_await transaction->execSqlCoro(CLAIM_COMMAND.data(),
                                            input.commandId,
                                            static_cast<int64_t>(
                                                normalized.size()),
                                            fingerprint, input.at);
      if (claimed.affectedRows() == 0) {
        const auto persisted =
            co_await transaction->execSqlCoro(FIND_COMMAND.data(),
                                              input.commandId);
        const bool committed =
            co_await TransactionCommitAwaiter(std::move(transaction));
        if (!committed)
          throw std::runtime_error("notification duplicate commit failed");
        const std::string stored =
            persisted.empty()
                ? std::string{}
                : persisted.front()["fingerprint"].as<std::string>();
        if (stored != fingerprint) {
          conflict = "command_id reused with a different payload";
        }
        else {
          duplicate = true;
          duplicateCount =
              persisted.empty()
                  ? 0
                  : persisted.front()["expected_count"].as<int64_t>();
        }
        if (conflict.has_value() || duplicate)
          ids.clear();
      }
    }
    if (!duplicate && !conflict.has_value()) {
      std::vector<std::string> args;
      const auto& argsRef = args;
      const auto inserted =
          co_await transaction->execSqlCoro(batchInsertSql(normalized, args),
                                            argsRef);
      ids.reserve(inserted.size());
      for (const auto& row : inserted)
        ids.push_back(row["id"].as<int64_t>());
      if (ids.size() != normalized.size())
        throw std::runtime_error(
            "notification batch insert returned wrong size");

      std::string deliverySql =
          "INSERT OR IGNORE INTO notification_delivery (notification_id, "
          "user_id, status, created_at, created_ms) VALUES ";
      std::vector<std::string> deliveryArgs;
      deliveryArgs.reserve(ids.size() * 4);
      const int64_t createdMs = nowMillis();
      for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index > 0)
          deliverySql += ", ";
        deliverySql += "(?, ?, ?, ?, ?)";
        deliveryArgs.push_back(std::to_string(ids[index]));
        deliveryArgs.push_back(std::to_string(normalized[index].userId));
        deliveryArgs.push_back(notificationDeliveryStatusToString(
            NotificationDeliveryStatus::Pending));
        deliveryArgs.push_back(std::to_string(input.at));
        deliveryArgs.push_back(std::to_string(createdMs));
      }
      const auto& deliveryArgsRef = deliveryArgs;
      co_await transaction->execSqlCoro(deliverySql, deliveryArgsRef);
    }
  }
  catch (const std::exception&) {
    if (transaction != nullptr)
      transaction->rollback();
    throw;
  }

  if (conflict.has_value())
    throw NotificationCommandConflict(*conflict);
  if (duplicate) {
    result.duplicate = true;
    result.expectedCount = duplicateCount;
    co_return result;
  }

  if (!co_await TransactionCommitAwaiter(std::move(transaction)))
    throw std::runtime_error("notification batch commit failed");
  result.expectedCount = static_cast<int64_t>(normalized.size());
  result.created = schemasFromIds({.inputs = normalized,
                                      .ids = ids,
                                      .now = std::time(nullptr)});
  co_return result;
}

drogon::Task<std::vector<NotificationDeliveryRow>>
NotificationRepository::pendingDeliveries(int limit) const
{
  auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(std::string(PENDING_DELIVERIES) +
                                       std::to_string(limit > 0 ? limit : 200),
                                   notificationDeliveryStatusToString(
                                       NotificationDeliveryStatus::Pending));
  std::vector<NotificationDeliveryRow> result;
  result.reserve(rows.size());
  for (const auto& row : rows) {
    result.push_back(
        {.deliveryId = row["id"].as<int64_t>(),
         .notificationId = row["notification_id"].as<int64_t>(),
         .userId = row["user_id"].as<int64_t>(),
         .type = row["type"].as<std::string>(),
         .title = row["title"].as<std::string>(),
         .body = row["body"].as<std::string>(),
         .data = json_util::fromString(row["data"].as<std::string>()),
         .createdAt = row["created_at"].as<int64_t>()});
  }
  co_return result;
}

drogon::Task<bool> NotificationRepository::markDelivered(int64_t deliveryId,
                                                         int64_t at) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(MARK_DELIVERED.data(),
                                   notificationDeliveryStatusToString(
                                       NotificationDeliveryStatus::Sent),
                                   at, nowMillis(), deliveryId,
                                   notificationDeliveryStatusToString(
                                       NotificationDeliveryStatus::Pending));
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t> NotificationRepository::ackDeliveries(
    const AckDeliveriesInput& input) const
{
  if (input.notificationIds.empty())
    co_return 0;
  std::string placeholders;
  std::vector<std::string> args;
  args.reserve(input.notificationIds.size() + 3);
  args.push_back(std::to_string(input.at));
  args.push_back(std::to_string(input.atMs));
  args.push_back(std::to_string(input.userId));
  for (size_t index = 0; index < input.notificationIds.size(); ++index) {
    if (index > 0)
      placeholders += ", ";
    placeholders += '?';
    args.push_back(std::to_string(input.notificationIds[index]));
  }
  std::string query{ACK_DELIVERIES};
  const auto position = query.find("%1%");
  if (position != std::string::npos)
    query.replace(position, 3, placeholders);
  auto client = DbService::client();
  const auto& argsRef = args;
  const auto result = co_await client->execSqlCoro(query, argsRef);
  co_return result.affectedRows();
}

drogon::Task<DeliverySummary> NotificationRepository::deliverySummary(
    const DeliverySummaryInput& input) const
{
  DeliverySummary summary;
  auto client = DbService::client();
  const auto counts = co_await client->execSqlCoro(DELIVERY_COUNTS.data(),
                                                   input.since);
  if (!counts.empty()) {
    summary.pending = counts.front()["pending"].as<int64_t>();
    summary.unacked = counts.front()["unacked"].as<int64_t>();
    summary.sent = counts.front()["sent"].as<int64_t>();
    summary.acked = counts.front()["acked"].as<int64_t>();
  }
  const int64_t oldBound = input.now - input.ackWindowS;
  const auto old = co_await client->execSqlCoro(DELIVERY_UNACKED_OLD.data(),
                                                oldBound);
  if (!old.empty())
    summary.unackedOld = old.front()["rows"].as<int64_t>();
  const auto latencyCount = co_await client->execSqlCoro(
      DELIVERY_LATENCY_COUNT.data(), input.since * 1000);
  const int64_t samples = latencyCount.empty()
                              ? 0
                              : latencyCount.front()["rows"].as<int64_t>();
  if (samples > 0) {
    const auto percentile = [&](double fraction) -> drogon::Task<int64_t> {
      const int64_t offset = std::min<int64_t>(
          samples - 1,
          static_cast<int64_t>(fraction * static_cast<double>(samples)));
      const auto rows = co_await client->execSqlCoro(
          DELIVERY_LATENCY_SAMPLE.data(), input.since * 1000, offset);
      co_return rows.empty() ? 0 : rows.front()["ms"].as<int64_t>();
    };
    summary.latencyMsP50 = co_await percentile(0.50);
    summary.latencyMsP95 = co_await percentile(0.95);
    const auto max = co_await client->execSqlCoro(DELIVERY_LATENCY_MAX.data(),
                                                  input.since * 1000);
    if (!max.empty())
      summary.latencyMsMax = max.front()["ms"].as<int64_t>();
  }
  const auto probe = co_await client->execSqlCoro(SELECT_SELFTEST.data());
  if (!probe.empty()) {
    summary.probeAt = probe.front()["last_at"].as<int64_t>();
    summary.probeOk = probe.front()["last_ok"].as<int>() != 0;
    summary.probeMs = probe.front()["last_ms"].as<int64_t>();
  }
  co_return summary;
}

drogon::Task<ProbeInsertResult> NotificationRepository::insertProbe(
    int64_t at, int64_t atMs) const
{
  ProbeInsertResult inserted;
  auto client = DbService::client();
  const auto notification =
      co_await client->execSqlCoro(INSERT_PROBE.data());
  if (notification.insertId() <= 0)
    co_return inserted;
  inserted.notificationId = notification.insertId();
  const auto delivery = co_await client->execSqlCoro(
      INSERT_PROBE_DELIVERY.data(), inserted.notificationId, at, atMs);
  inserted.deliveryId = delivery.insertId();
  co_return inserted;
}

drogon::Task<std::string> NotificationRepository::deliveryState(
    int64_t deliveryId) const
{
  auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(DELIVERY_STATE.data(), deliveryId);
  if (rows.empty())
    co_return std::string{};
  co_return rows.front()["status"].as<std::string>();
}

drogon::Task<SelfTestState> NotificationRepository::probeState() const
{
  SelfTestState state;
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(SELECT_SELFTEST.data());
  if (rows.empty())
    co_return state;
  state.at = rows.front()["last_at"].as<int64_t>();
  state.ok = rows.front()["last_ok"].as<int>() != 0;
  state.ms = rows.front()["last_ms"].as<int64_t>();
  co_return state;
}

drogon::Task<bool> NotificationRepository::recordProbe(
    const ProbeRecordInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      UPSERT_SELFTEST.data(), input.at, input.ok ? 1 : 0, input.ms);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t> NotificationRepository::purgeProbes(
    int64_t olderThan) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(PURGE_PROBE_DELIVERIES.data(), olderThan);
  const auto result =
      co_await client->execSqlCoro(PURGE_PROBES.data(), olderThan);
  co_return result.affectedRows();
}

drogon::Task<int64_t> NotificationRepository::pendingDeliveryCount() const
{
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(
      PENDING_DELIVERY_COUNT.data(),
      notificationDeliveryStatusToString(NotificationDeliveryStatus::Pending));
  if (rows.empty())
    co_return 0;
  co_return rows.front()["total"].as<int64_t>();
}

drogon::Task<std::vector<Json::Value>>
NotificationRepository::findSync(const NotificationSyncFilter& filter) const
{
  auto client = DbService::client();
  if (filter.startTime && filter.startId && filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_AFTER) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId, *filter.startTime,
                                     *filter.startTime, *filter.startId,
                                     *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime && filter.startId) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_AFTER_FROM) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId, *filter.startTime,
                                     *filter.startTime, *filter.startId);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime && filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId, *filter.startTime,
                                     *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_FROM) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId, *filter.startTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  if (filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_TO) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
  {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_ALL) +
                                         AppConfig::SYNC_LIMIT,
                                     filter.userId);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(NotificationSchema(row).toJson());
    co_return data;
  }
}

drogon::Task<std::optional<Json::Value>>
NotificationRepository::findLastSync(const NotificationSyncFilter& filter) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_SYNC.data(), filter.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return NotificationSchema(result.front()).toJson();
}

drogon::Task<std::vector<NotificationReadChange>>
NotificationRepository::markAsRead(int64_t userId,
                                   const std::vector<int64_t>& ids) const
{
  if (ids.empty())
    co_return {};

  std::string placeholders;
  std::vector<std::string> args;
  args.reserve(ids.size() + 1);
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0)
      placeholders += ", ";
    placeholders += '?';
    args.push_back(std::to_string(ids[i]));
  }
  args.insert(args.begin(), std::to_string(userId));

  const auto withIds = [&placeholders](std::string_view templateQuery) {
    std::string query{templateQuery};
    const auto position = query.find("%1%");
    if (position != std::string::npos)
      query.replace(position, 3, placeholders);
    return query;
  };

  auto client = DbService::client();
  const auto& argsRef = args;
  const auto rows =
      co_await client->execSqlCoro(withIds(FIND_UNREAD_BY_IDS), argsRef);
  if (rows.empty())
    co_return {};

  std::vector<NotificationReadChange> changes;
  changes.reserve(rows.size());
  const auto readAt = static_cast<int64_t>(std::time(nullptr));
  for (const auto& row : rows) {
    NotificationReadChange change;
    change.before = NotificationSchema(row);
    change.after = change.before;
    change.after.isRead = true;
    change.after.readAt = readAt;
    changes.push_back(std::move(change));
  }

  co_await client->execSqlCoro(withIds(MARK_READ), argsRef);
  co_return changes;
}
