#pragma once

#include <feature/guard/vocabulary/feedback-label.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace episode_query
{

inline constexpr std::string_view LIST_EPISODES =
    "SELECT e.id, e.state, e.checks, e.best_camera_id, e.first_seen, "
    "e.last_seen, e.notify_count, e.notify_highest_rank, e.subject, e.people, "
    "e.reasons, e.reasons_rank, e.group_id, e.review_label, e.reviewed_at, "
    "(SELECT t.reason FROM guard_encounter_transition t WHERE t.encounter_id "
    "= e.id ORDER BY t.id DESC LIMIT 1) AS last_reason, "
    "EXISTS (SELECT 1 FROM guard_action a WHERE a.encounter_id = e.id AND "
    "a.kind IN ('greet', 'greet_reply', 'announce') AND a.status IN "
    "('succeeded', 'duplicate_succeeded')) AS spoke, "
    "EXISTS (SELECT 1 FROM guard_action a WHERE a.encounter_id = e.id AND "
    "a.kind IN ('alarm', 'siren_arm') AND a.status IN ('succeeded', "
    "'duplicate_succeeded')) AS sounded "
    "FROM guard_encounter e WHERE (? = 0 OR e.last_seen < ?) "
    "ORDER BY e.last_seen DESC, e.id DESC LIMIT ?";

inline constexpr std::string_view FIND_EPISODE =
    "SELECT e.id, e.state, e.checks, e.best_camera_id, e.first_seen, "
    "e.last_seen, e.notify_count, e.notify_highest_rank, e.subject, e.people, "
    "e.reasons, e.reasons_rank, e.group_id, e.review_label, e.reviewed_at, "
    "(SELECT t.reason FROM guard_encounter_transition t WHERE t.encounter_id "
    "= e.id ORDER BY t.id DESC LIMIT 1) AS last_reason, "
    "EXISTS (SELECT 1 FROM guard_action a WHERE a.encounter_id = e.id AND "
    "a.kind IN ('greet', 'greet_reply', 'announce') AND a.status IN "
    "('succeeded', 'duplicate_succeeded')) AS spoke, "
    "EXISTS (SELECT 1 FROM guard_action a WHERE a.encounter_id = e.id AND "
    "a.kind IN ('alarm', 'siren_arm') AND a.status IN ('succeeded', "
    "'duplicate_succeeded')) AS sounded "
    "FROM guard_encounter e WHERE e.id = ?";

inline constexpr std::string_view EPISODE_TIMELINE =
    "SELECT 'state' AS entry, occurred_at AS at, to_state AS what, "
    "reason AS detail, '[]' AS reasons, 0 AS notified, 0 AS tier, id AS seq "
    "FROM guard_encounter_transition WHERE encounter_id = ? "
    "UNION ALL SELECT 'decision', created_at, severity, suppression_reason, "
    "reasons, did_notify, 1, rowid FROM guard_decision_journal "
    "WHERE encounter_id = ? AND event_id NOT LIKE 'encounter:%' "
    "UNION ALL SELECT 'action', created_at, kind, status, '[]', 0, 2, id "
    "FROM guard_action WHERE encounter_id = ? AND kind NOT LIKE 'agent\\_%' "
    "ESCAPE '\\' ORDER BY at ASC, tier ASC, seq ASC LIMIT 400";

inline constexpr std::string_view UPDATE_INSIGHT =
    "UPDATE guard_encounter SET subject = ?, people = MAX(people, ?), "
    "reasons = CASE WHEN ? >= reasons_rank THEN ? ELSE reasons END, "
    "reasons_rank = MAX(reasons_rank, ?) WHERE id = ?";

inline constexpr std::string_view LINK_GROUP =
    "UPDATE guard_encounter SET group_id = ? WHERE id = ? AND group_id = 0";

inline constexpr std::string_view REVIEW_EPISODE =
    "UPDATE guard_encounter SET review_label = ?, reviewed_at = ? "
    "WHERE id = ?";

inline constexpr std::string_view REVIEW_DECISIONS =
    "UPDATE guard_decision_journal SET feedback_label = ?, feedback_at = ? "
    "WHERE encounter_id = ? AND did_notify = 1";

inline constexpr std::string_view RECENT_CAMERA_NOTIFICATION =
    "SELECT encounter_id, MAX(severity_rank) AS rank FROM "
    "guard_decision_journal WHERE camera_id = ? AND did_notify = 1 AND "
    "encounter_id > 0 AND encounter_id != ? AND created_at >= ? "
    "GROUP BY encounter_id ORDER BY rank DESC, MAX(created_at) DESC LIMIT 1";

inline constexpr std::string_view DIGEST_WINDOW =
    "WITH episode AS (SELECT j.encounter_id, j.camera_id, "
    "MAX(CASE WHEN j.did_notify = 1 OR j.suppression_reason = 'grouped' "
    "THEN 1 ELSE 0 END) AS notified, "
    "MAX(CASE WHEN j.suppression_reason = 'held' THEN 1 ELSE 0 END) AS held, "
    "MAX(j.incident_id) AS incident_id FROM guard_decision_journal j "
    "WHERE j.created_at > ? AND j.created_at <= ? AND j.encounter_id > 0 "
    "GROUP BY j.encounter_id, j.camera_id) "
    "SELECT e.camera_id, COALESCE(MAX(i.camera_name), '') AS camera_name, "
    "SUM(e.notified) AS notified, "
    "SUM(CASE WHEN e.held = 1 AND e.notified = 0 THEN 1 ELSE 0 END) AS held, "
    "SUM(CASE WHEN e.held = 0 AND e.notified = 0 THEN 1 ELSE 0 END) AS routine "
    "FROM episode e LEFT JOIN guard_incident i ON i.id = e.incident_id "
    "GROUP BY e.camera_id";

inline constexpr std::string_view LIST_TAMPER =
    "SELECT id, camera_id, camera_name, danger, event_json, created_at FROM "
    "guard_incident WHERE rule = 'camera_tamper' AND (? = 0 OR created_at < ?) "
    "ORDER BY created_at DESC LIMIT ?";

inline constexpr std::string_view OPEN_TAMPER_KEYS =
    "SELECT key, value FROM guard_state WHERE key LIKE 'tamper\\_onset\\_%' "
    "ESCAPE '\\'";

}

struct EpisodeRow
{
  int64_t id{0};
  std::string state;
  int checks{0};
  int64_t cameraId{0};
  int64_t firstSeen{0};
  int64_t lastSeen{0};
  int notifyCount{0};
  int notifyHighestRank{0};
  std::string subject;
  int people{0};
  std::string reasons;
  int reasonsRank{0};
  int64_t groupId{0};
  std::string reviewLabel;
  int64_t reviewedAt{0};
  std::string lastReason;
  bool spoke{false};
  bool sounded{false};
};

struct EpisodeListInput
{
  int limit{30};
  int64_t before{0};
};

struct EpisodeTimelineRow
{
  std::string entry;
  int64_t at{0};
  std::string what;
  std::string detail;
  std::string reasons;
  bool notified{false};
};

struct EpisodeInsightInput
{
  int64_t encounterId{0};
  std::string subject;
  int people{0};
  std::string reasons;
  int rank{0};
};

struct EpisodeReviewInput
{
  int64_t encounterId{0};
  FeedbackLabel label{FeedbackLabel::Useful};
  int64_t at{0};
};

struct CameraNotificationLookupInput
{
  int64_t cameraId{0};
  int64_t excludeEncounterId{0};
  int64_t since{0};
};

struct CameraNotification
{
  int64_t encounterId{0};
  int rank{0};
};

struct DigestWindowInput
{
  int64_t from{0};
  int64_t to{0};
};

struct DigestRow
{
  int64_t cameraId{0};
  std::string cameraName;
  int64_t notified{0};
  int64_t held{0};
  int64_t routine{0};
};

struct TamperRow
{
  int64_t incidentId{0};
  int64_t cameraId{0};
  std::string cameraName;
  std::string danger;
  std::string status;
  int64_t createdAt{0};
  bool open{false};
};
