#pragma once

#include <array>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace module_data_query
{

inline constexpr std::string_view SUMMARY =
    "SELECT "
    "(SELECT COUNT(*) FROM camera WHERE deleted_at IS NULL) AS cameras, "
    "(SELECT COUNT(*) FROM zone WHERE deleted_at IS NULL) AS zones, "
    "(SELECT COUNT(*) FROM camera_evidence WHERE deleted_at = 0) AS evidence, "
    "(SELECT COUNT(*) FROM action_command) AS actions, "
    "(SELECT COALESCE(SUM(?1 + length(name) + length(ip) + length(username) + length(password) + "
    "length(cloud_username) + length(cloud_password) + length(capabilities) + length(config)), 0) FROM camera) + "
    "(SELECT COALESCE(SUM(?1 + length(url)), 0) FROM camera_stream) + "
    "(SELECT COALESCE(SUM(?1 + length(name) + length(points)), 0) FROM zone) + "
    "(SELECT COALESCE(SUM(?1 + length(object_key)), 0) FROM camera_evidence) + "
    "(SELECT COALESCE(SUM(?1 + length(detail) + length(response)), 0) FROM action_command) + "
    "(SELECT COALESCE(SUM(?1 + length(payload)), 0) FROM object_event_outbox) + "
    "(SELECT COALESCE(SUM(?1 + length(payload)), 0) FROM change_outbox) AS bytes";

inline constexpr std::string_view CAMERA_IDS = "SELECT id FROM camera";

inline constexpr std::array<std::string_view, 10> PURGE{
    "UPDATE camera_evidence SET expires_at = 1 WHERE deleted_at = 0",
    "DELETE FROM camera_evidence WHERE deleted_at > 0",
    "DELETE FROM zone",
    "DELETE FROM camera_stream",
    "DELETE FROM camera",
    "DELETE FROM action_command",
    "DELETE FROM siren_lease",
    "DELETE FROM object_event_outbox",
    "DELETE FROM camera_event_cooldown",
    "DELETE FROM change_outbox"};

inline constexpr std::string_view PENDING_EVIDENCE =
    "SELECT id, object_key FROM camera_evidence WHERE deleted_at = 0 ORDER BY id LIMIT ?";

inline constexpr std::string_view COUNT_PENDING_EVIDENCE =
    "SELECT COUNT(*) AS total FROM camera_evidence WHERE deleted_at = 0";

inline constexpr std::string_view DELETE_EVIDENCE = "DELETE FROM camera_evidence WHERE id = ?";

inline constexpr std::int64_t kRowOverheadBytes = 64;
inline constexpr std::int64_t kEvidenceBatch = 200;

}

struct PendingEvidence
{
  std::int64_t id{0};
  std::string objectKey;
};
