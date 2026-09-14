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

inline constexpr std::string_view MARK_DELIVERED =
    "UPDATE notification_delivery SET status = ?, "
    "attempts = attempts + 1, sent_at = ? WHERE id = ? AND status = ?";

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
