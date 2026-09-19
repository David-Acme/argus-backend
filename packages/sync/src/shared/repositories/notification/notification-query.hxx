#pragma once
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/schemas/notification/notification-schema.hxx>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace notification_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO notification (user_id, type, title, body, data) "
    "VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view INSERT_MANY_PREFIX =
    "INSERT INTO notification (user_id, type, title, body, data) VALUES ";

inline constexpr std::string_view INSERT_MANY_SUFFIX = " RETURNING id";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_FROM =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT ";
inline constexpr std::string_view FIND_SYNC_AFTER =
    "SELECT * FROM notification WHERE user_id = ? AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT ";
inline constexpr std::string_view FIND_SYNC_AFTER_FROM =
    "SELECT * FROM notification WHERE user_id = ? AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_TO =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at <= ? ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_ALL =
    "SELECT * FROM notification WHERE user_id = ? "
    "ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM notification WHERE user_id = ? "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view MARK_READ =
    "UPDATE notification SET is_read = 1, read_at = strftime('%s', 'now') "
    "WHERE user_id = ? AND is_read = 0 AND id IN (%1%)";

inline constexpr std::string_view FIND_UNREAD_BY_IDS =
    "SELECT * FROM notification WHERE user_id = ? AND is_read = 0 AND id IN "
    "(%1%) "
    "ORDER BY id ASC";
} // namespace notification_query

inline constexpr std::string_view CLAIM_COMMAND =
    "INSERT OR IGNORE INTO notification_command (command_id, expected_count, "
    "fingerprint, created_at) VALUES (?, ?, ?, ?)";

inline constexpr std::string_view FIND_COMMAND =
    "SELECT expected_count, fingerprint FROM notification_command "
    "WHERE command_id = ?";

struct NotificationCommandConflict : public std::runtime_error
{
  explicit NotificationCommandConflict(const std::string& message)
      : std::runtime_error(message)
  {
  }
};

struct NotificationValidationError : public std::runtime_error
{
  explicit NotificationValidationError(const std::string& message)
      : std::runtime_error(message)
  {
  }
};

inline constexpr std::size_t kMaxNotificationRecipients = 1000;
inline constexpr std::size_t kMaxNotificationType = 32;
inline constexpr std::size_t kMaxNotificationTitle = 200;
inline constexpr std::size_t kMaxNotificationBody = 2000;
inline constexpr std::size_t kMaxNotificationData = 8192;

inline constexpr std::string_view PENDING_DELIVERIES =
    "SELECT d.id, d.notification_id, d.user_id, n.type, n.title, n.body, "
    "n.data, n.created_at FROM notification_delivery d "
    "JOIN notification n ON n.id = d.notification_id "
    "WHERE d.status = ? ORDER BY d.id ASC LIMIT ";

inline constexpr std::string_view PENDING_DELIVERY_COUNT =
    "SELECT COUNT(*) AS total FROM notification_delivery WHERE status = ?";

inline constexpr std::string_view MARK_DELIVERED =
    "UPDATE notification_delivery SET status = ?, "
    "attempts = attempts + 1, sent_at = ?, sent_ms = ? WHERE id = ? AND "
    "status = ?";

inline constexpr std::string_view ACK_DELIVERIES =
    "UPDATE notification_delivery SET acked_at = ?, acked_ms = ? "
    "WHERE user_id = ? AND status = 'sent' AND acked_at = 0 AND "
    "notification_id IN (%1%)";

inline constexpr std::string_view DELIVERY_STATE =
    "SELECT status FROM notification_delivery WHERE id = ?";

inline constexpr std::string_view DELIVERY_COUNTS =
    "SELECT COUNT(*) AS rows, "
    "COALESCE(SUM(status = 'pending'), 0) AS pending, "
    "COALESCE(SUM(status = 'sent' AND acked_at = 0), 0) AS unacked, "
    "COALESCE(SUM(status = 'sent'), 0) AS sent, "
    "COALESCE(SUM(acked_at > 0), 0) AS acked "
    "FROM notification_delivery WHERE created_at >= ? AND notification_id NOT "
    "IN (SELECT id FROM notification WHERE type = 'probe')";

inline constexpr std::string_view DELIVERY_UNACKED_OLD =
    "SELECT COUNT(*) AS rows FROM notification_delivery WHERE status = 'sent' "
    "AND acked_at = 0 AND sent_at > 0 AND sent_at <= ? AND notification_id "
    "NOT IN (SELECT id FROM notification WHERE type = 'probe')";

inline constexpr std::string_view DELIVERY_LATENCY_COUNT =
    "SELECT COUNT(*) AS rows FROM notification_delivery WHERE status = 'sent' "
    "AND sent_ms > created_ms AND created_ms >= ?";

inline constexpr std::string_view DELIVERY_LATENCY_SAMPLE =
    "SELECT (sent_ms - created_ms) AS ms FROM notification_delivery WHERE "
    "status = 'sent' AND sent_ms > created_ms AND created_ms >= ? "
    "ORDER BY ms ASC LIMIT 1 OFFSET ?";

inline constexpr std::string_view DELIVERY_LATENCY_MAX =
    "SELECT COALESCE(MAX(sent_ms - created_ms), 0) AS ms FROM "
    "notification_delivery WHERE status = 'sent' AND sent_ms > created_ms AND "
    "created_ms >= ?";

inline constexpr std::string_view INSERT_PROBE =
    "INSERT INTO notification (user_id, type, title, body, data) VALUES "
    "(0, 'probe', 'delivery probe', '', '{}')";

inline constexpr std::string_view INSERT_PROBE_DELIVERY =
    "INSERT INTO notification_delivery (notification_id, user_id, status, "
    "created_at, created_ms) VALUES (?, 0, 'pending', ?, ?)";

inline constexpr std::string_view UPSERT_SELFTEST =
    "INSERT INTO notification_selftest (id, last_at, last_ok, last_ms) VALUES "
    "(1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET last_at = excluded.last_at, "
    "last_ok = excluded.last_ok, last_ms = excluded.last_ms";

inline constexpr std::string_view SELECT_SELFTEST =
    "SELECT last_at, last_ok, last_ms FROM notification_selftest WHERE id = 1";

inline constexpr std::string_view PURGE_PROBE_DELIVERIES =
    "DELETE FROM notification_delivery WHERE notification_id IN (SELECT id "
    "FROM notification WHERE type = 'probe' AND created_at < ?)";

inline constexpr std::string_view PURGE_PROBES =
    "DELETE FROM notification WHERE type = 'probe' AND created_at < ?";

struct NotificationCreateInput
{
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
};

struct NotificationBatchCommitInput
{
  std::vector<NotificationCreateInput> inputs;
  std::string commandId;
  int64_t at{0};
};

struct NotificationSchemaBuildInput
{
  std::vector<NotificationCreateInput> inputs;
  std::vector<int64_t> ids;
  int64_t now{0};
};

struct NotificationCommitResult
{
  bool duplicate{false};
  int64_t expectedCount{0};
  std::vector<NotificationSchema> created;
};

struct NotificationDeliveryRow
{
  int64_t deliveryId{0};
  int64_t notificationId{0};
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
  int64_t createdAt{0};
};

struct NotificationSyncFilter
{
  int64_t userId{0};
  std::optional<int64_t> startTime;
  std::optional<int64_t> startId;
  std::optional<int64_t> endTime;
};

struct AckDeliveriesInput
{
  int64_t userId{0};
  std::vector<int64_t> notificationIds;
  int64_t at{0};
  int64_t atMs{0};
};

struct DeliverySummaryInput
{
  int64_t since{0};
  int64_t ackWindowS{86400};
  int64_t now{0};
};

struct DeliverySummary
{
  int64_t pending{0};
  int64_t unacked{0};
  int64_t unackedOld{0};
  int64_t sent{0};
  int64_t acked{0};
  int64_t latencyMsP50{0};
  int64_t latencyMsP95{0};
  int64_t latencyMsMax{0};
  int64_t probeAt{0};
  bool probeOk{false};
  int64_t probeMs{0};
};

struct ProbeInsertResult
{
  int64_t notificationId{0};
  int64_t deliveryId{0};
};

struct ProbeRecordInput
{
  int64_t at{0};
  bool ok{false};
  int64_t ms{0};
};

struct SelfTestState
{
  int64_t at{0};
  bool ok{false};
  int64_t ms{0};
};
