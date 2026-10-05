#pragma once

#include <feature/call/vocabulary/response-state.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace call_response_query
{
inline constexpr std::string_view INSERT =
    "INSERT OR IGNORE INTO call_response (dedupe_key, kind, environment_id, "
    "episode_id, camera_id, strategy, state, step, step_count, step_seconds, "
    "step_deadline, plan, data, created_at, updated_at) "
    "VALUES (?, ?, ?, ?, ?, ?, 'active', 0, ?, ?, ?, ?, ?, ?, ?) RETURNING id";

inline constexpr std::string_view INSERT_MEMBERS_HEAD =
    "INSERT OR IGNORE INTO call_response_member (response_id, user_id, step, "
    "mode, mandatory, discreet, reached_at) VALUES ";

inline constexpr std::string_view INSERT_MEMBER_ROW = "(?, ?, ?, ?, ?, ?, 0)";

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM call_response WHERE id = ?";

inline constexpr std::string_view FIND_BY_KEY =
    "SELECT * FROM call_response WHERE dedupe_key = ?";

inline constexpr std::string_view MEMBERS =
    "SELECT * FROM call_response_member WHERE response_id = ? "
    "ORDER BY step ASC, user_id ASC";

inline constexpr std::string_view MEMBER =
    "SELECT * FROM call_response_member WHERE response_id = ? AND user_id = ?";

inline constexpr std::string_view MARK_REACHED =
    "UPDATE call_response_member SET reached_at = ? "
    "WHERE response_id = ? AND user_id = ? AND reached_at = 0";

inline constexpr std::string_view DUE =
    "SELECT * FROM call_response WHERE state = 'active' AND step_deadline <= ? "
    "ORDER BY id ASC LIMIT ?";

inline constexpr std::string_view ADVANCE =
    "UPDATE call_response SET step = ?, step_deadline = ?, updated_at = ? "
    "WHERE id = ? AND state = 'active' AND step = ?";

inline constexpr std::string_view SET_DEADLINE =
    "UPDATE call_response SET step_deadline = ?, updated_at = ? "
    "WHERE id = ? AND state = 'active' AND step = ?";

inline constexpr std::string_view MARK_UNANSWERED =
    "UPDATE call_response SET state = 'unanswered', updated_at = ? "
    "WHERE id = ? AND state = 'active'";

inline constexpr std::string_view ATTEND =
    "UPDATE call_response SET state = 'attended', responder_id = ?, "
    "responder_name = ?, updated_at = ? WHERE id = ? "
    "AND state IN ('active', 'unanswered') AND responder_id = 0";

inline constexpr std::string_view VERDICT_HEAD =
    "UPDATE call_response SET state = ?, verdict = ?, verdict_by = ?, "
    "verdict_by_name = ?, verdict_at = ?, "
    "responder_name = CASE WHEN responder_id = 0 THEN ? ELSE responder_name END, "
    "responder_id = CASE WHEN responder_id = 0 THEN ? ELSE responder_id END, "
    "updated_at = ? WHERE id = ? AND state IN ";

inline constexpr std::string_view VERDICT_FROM_OPEN =
    "('active', 'attended', 'unanswered')";

inline constexpr std::string_view VERDICT_FROM_CONFIRMED =
    "('active', 'attended', 'unanswered', 'confirmed')";

inline constexpr std::string_view EXPIRE =
    "UPDATE call_response SET state = 'expired', updated_at = ? "
    "WHERE state IN ('active', 'attended', 'unanswered', 'confirmed') "
    "AND created_at < ? RETURNING id";

inline constexpr std::string_view FOR_USER =
    "SELECT r.* FROM call_response r JOIN call_response_member m "
    "ON m.response_id = r.id WHERE m.user_id = ? AND m.reached_at > 0 "
    "AND (r.state IN ('active', 'attended', 'unanswered', 'confirmed') "
    "OR r.updated_at >= ?) ORDER BY r.id DESC LIMIT ?";
}

struct CallResponseCreateInput
{
  std::string dedupeKey;
  std::string kind;
  int64_t environmentId{0};
  int64_t episodeId{0};
  int64_t cameraId{0};
  std::string strategy;
  int stepCount{1};
  int stepSeconds{45};
  int64_t stepDeadline{0};
  std::string plan;
  std::string data;
  int64_t at{0};
};

struct CallResponseMemberInput
{
  int64_t userId{0};
  int step{0};
  ResponseMemberMode mode{ResponseMemberMode::Call};
  bool mandatory{false};
  bool discreet{false};
};

struct CallResponseReachInput
{
  int64_t responseId{0};
  int64_t userId{0};
  int64_t at{0};
};

struct CallResponseAdvanceInput
{
  int64_t id{0};
  int fromStep{0};
  int toStep{0};
  int64_t deadline{0};
  int64_t at{0};
};

struct CallResponseAttendInput
{
  int64_t id{0};
  int64_t userId{0};
  std::string name;
  int64_t at{0};
};

struct CallResponseVerdictInput
{
  int64_t id{0};
  ResponseVerdict verdict{ResponseVerdict::FalseAlarm};
  int64_t userId{0};
  std::string name;
  int64_t at{0};
};

struct CallResponseDueInput
{
  int64_t now{0};
  int limit{50};
};

struct CallResponseUserInput
{
  int64_t userId{0};
  int64_t closedSince{0};
  int limit{20};
};
