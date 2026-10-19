#pragma once

#include <auth/user-role.hxx>
#include <feature/guard/maintenance-schema.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace maintenance_query
{

inline constexpr std::string_view INSERT_WINDOW =
    "INSERT INTO guard_maintenance (environment_id, camera_ids, declared_by, "
    "declared_role, declared_name, source, duress, started_at, ends_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_WINDOW =
    "SELECT * FROM guard_maintenance WHERE id = ?";

inline constexpr std::string_view LIST_OPEN =
    "SELECT * FROM guard_maintenance WHERE ended_at = 0 ORDER BY id ASC";

inline constexpr std::string_view LIST_RECENT =
    "SELECT * FROM guard_maintenance WHERE ended_at = 0 OR ended_at >= ? "
    "ORDER BY started_at DESC, id DESC LIMIT 100";

inline constexpr std::string_view END_WINDOW =
    "UPDATE guard_maintenance SET ended_at = ?, end_reason = ? "
    "WHERE id = ? AND ended_at = 0";

inline constexpr std::string_view SET_VIEW_CHECKS =
    "UPDATE guard_maintenance SET view_checks = ? WHERE id = ?";

inline constexpr std::string_view RECENT_DURESS =
    "SELECT COUNT(*) AS total FROM guard_safety_alert "
    "WHERE user_id = ? AND kind = 'duress' AND created_at >= ?";

inline constexpr std::string_view INSERT_ASK =
    "INSERT INTO guard_tamper_ask (camera_id, environment_id, onset_at, asked_at, "
    "asked_users) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_ASK =
    "SELECT * FROM guard_tamper_ask WHERE id = ?";

inline constexpr std::string_view FIND_ASK_FOR_ONSET =
    "SELECT * FROM guard_tamper_ask WHERE camera_id = ? AND onset_at = ? "
    "ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view PENDING_ASKS =
    "SELECT * FROM guard_tamper_ask WHERE answer = '' ORDER BY id ASC";

inline constexpr std::string_view FIND_INCIDENT =
    "SELECT id, camera_id, rule FROM guard_incident WHERE id = ?";

inline constexpr std::string_view ANSWER_ASK =
    "UPDATE guard_tamper_ask SET answer = ?, answered_by = ?, answered_at = ? "
    "WHERE id = ? AND answer = ''";

}

struct MaintenanceCreateInput
{
  int64_t environmentId{0};
  std::vector<int64_t> cameraIds;
  int64_t declaredBy{0};
  UserRole declaredRole{UserRole::Unknown};
  std::string declaredName;
  MaintenanceSource source{MaintenanceSource::App};
  bool duress{false};
  int64_t startedAt{0};
  int64_t endsAt{0};
};

struct MaintenanceEndInput
{
  int64_t id{0};
  MaintenanceEndReason reason{MaintenanceEndReason::Done};
  int64_t at{0};
};

struct MaintenanceViewChecksInput
{
  int64_t id{0};
  std::vector<ViewCheck> checks;
};

struct DuressLookupInput
{
  int64_t userId{0};
  int64_t since{0};
};

struct TamperAskCreateInput
{
  int64_t cameraId{0};
  int64_t environmentId{0};
  int64_t onsetAt{0};
  int64_t askedAt{0};
  std::vector<int64_t> askedUsers;
};

struct TamperAskOnsetInput
{
  int64_t cameraId{0};
  int64_t onsetAt{0};
};

struct IncidentRef
{
  int64_t id{0};
  int64_t cameraId{0};
  std::string rule;
};

struct TamperAskAnswerInput
{
  int64_t id{0};
  TamperAskAnswer answer{TamperAskAnswer::Timeout};
  int64_t answeredBy{0};
  int64_t at{0};
};
