#pragma once
#include <cstdint>
#include <json/value.h>
#include <shared/enums.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <string>
#include <string_view>
#include <vector>

namespace guard_query
{
inline constexpr std::string_view INSERT_INCIDENT =
    "INSERT INTO guard_incident (camera_id, camera_name, rule, danger, "
    "severity, "
    "person_id, identity, event_id, event_json, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view INSERT_INCIDENT_EVENT =
    "INSERT OR IGNORE INTO guard_incident (camera_id, camera_name, rule, "
    "danger, "
    "severity, person_id, identity, event_id, event_json, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view SELECT_INCIDENT_BY_EVENT =
    "SELECT id FROM guard_incident WHERE event_id = ?";

inline constexpr std::string_view UPDATE_INCIDENT_DANGER =
    "UPDATE guard_incident SET danger = ? WHERE id = ?";

inline constexpr std::string_view INSERT_ACTION =
    "INSERT OR IGNORE INTO guard_action (incident_id, encounter_id, camera_id, "
    "person_id, command_id, kind, status, detail, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view COUNT_PERSON_SINCE =
    "SELECT COUNT(*) AS total FROM guard_incident "
    "WHERE person_id = ? AND created_at >= ?";

inline constexpr std::string_view LAST_ACTION =
    "SELECT created_at FROM guard_action WHERE camera_id = ? AND person_id = ? "
    "AND kind IN ('announce', 'alarm', 'siren_arm', 'notify') "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view COUNT_ACTIONS_SINCE =
    "SELECT COUNT(*) AS total FROM guard_action WHERE camera_id = ? "
    "AND kind IN ('announce', 'alarm', 'siren_arm', 'notify') "
    "AND created_at >= ?";

inline constexpr std::string_view COUNT_ENCOUNTER_ACTIONS_SINCE =
    "SELECT COUNT(*) AS total FROM guard_action WHERE encounter_id = ? "
    "AND kind IN ('announce', 'alarm', 'siren_arm', 'notify') "
    "AND created_at >= ?";

inline constexpr std::string_view SEL_STATE =
    "SELECT value FROM guard_state WHERE key = ?";

inline constexpr std::string_view UPSERT_STATE =
    "INSERT INTO guard_state (key, value, updated_at) VALUES (?, ?, ?) "
    "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
    "updated_at = excluded.updated_at";

inline constexpr std::string_view SELECT_RECENT_INCIDENTS =
    "SELECT camera_id, camera_name, rule, danger, severity, person_id, "
    "identity, "
    "created_at FROM guard_incident ORDER BY created_at DESC LIMIT ?";

inline constexpr std::string_view INSERT_GUEST =
    "INSERT INTO guard_expected_guest (description, camera_id, person_id, "
    "host_user_id, one_time, valid_from, valid_until, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view ACTIVE_GUEST =
    "SELECT id, description, camera_id, person_id, host_user_id, one_time "
    "FROM guard_expected_guest "
    "WHERE deleted_at IS NULL AND used_at = 0 AND valid_from <= ? AND "
    "valid_until >= ? AND (camera_id = 0 OR camera_id = ?) "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view LIST_GUESTS =
    "SELECT id, description, camera_id, person_id, host_user_id, one_time, "
    "valid_from, valid_until FROM guard_expected_guest "
    "WHERE deleted_at IS NULL ORDER BY created_at DESC LIMIT 50";

inline constexpr std::string_view CONSUME_GUEST =
    "UPDATE guard_expected_guest SET used_at = ? WHERE id = ? AND used_at = 0";

inline constexpr std::string_view DELETE_GUEST =
    "UPDATE guard_expected_guest SET deleted_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view INSERT_ENCOUNTER =
    "INSERT INTO guard_encounter (person_id, signature, state, grade, checks, "
    "best_camera_id, best_score, first_seen, last_seen) "
    "VALUES (?, ?, 'observing', 'none', 0, ?, ?, ?, ?)";

inline constexpr std::string_view OPEN_ENCOUNTERS =
    "SELECT id, person_id, signature, state, grade, checks, best_camera_id, "
    "best_score, dialogue_turns, dialogue_goal, listening_until, last_line, "
    "last_heard, last_heard_at, first_seen, last_seen FROM guard_encounter "
    "WHERE state != 'closed' AND last_seen >= ? ORDER BY last_seen DESC LIMIT "
    "50";

inline constexpr std::string_view STALE_ENCOUNTERS =
    "SELECT id, person_id, signature, state, grade, checks, best_camera_id, "
    "best_score, dialogue_turns, dialogue_goal, listening_until, last_line, "
    "last_heard, last_heard_at, first_seen, last_seen FROM guard_encounter "
    "WHERE state != 'closed' AND last_seen < ?";

inline constexpr std::string_view ENCOUNTERS_FOR_PERSON =
    "SELECT id, person_id, signature, state, grade, checks, best_camera_id, "
    "best_score, dialogue_turns, dialogue_goal, listening_until, last_line, "
    "last_heard, last_heard_at, first_seen, last_seen FROM guard_encounter "
    "WHERE person_id = ? AND state != 'closed'";

inline constexpr std::string_view FIND_ENCOUNTER =
    "SELECT id, person_id, signature, state, grade, checks, best_camera_id, "
    "best_score, dialogue_turns, dialogue_goal, listening_until, last_line, "
    "last_heard, last_heard_at, first_seen, last_seen FROM guard_encounter "
    "WHERE id = ?";

inline constexpr std::string_view SET_DIALOGUE_GOAL =
    "UPDATE guard_encounter SET dialogue_goal = ?, listening_until = ?, "
    "last_line = ?, last_heard = ? WHERE id = ?";

inline constexpr std::string_view RECORD_DIALOGUE =
    "UPDATE guard_encounter SET dialogue_turns = dialogue_turns + "
    "(CASE WHEN dialogue_turn_key = ? THEN 0 ELSE 1 END), "
    "dialogue_turn_key = ?, dialogue_goal = ?, listening_until = ?, "
    "last_line = ?, last_heard = ?, last_heard_at = ? WHERE id = ?";

inline constexpr std::string_view TOUCH_ENCOUNTER =
    "UPDATE guard_encounter SET checks = checks + 1, best_camera_id = ?, "
    "best_score = ?, last_seen = ? WHERE id = ? RETURNING checks";

inline constexpr std::string_view SET_ENCOUNTER_STATE =
    "UPDATE guard_encounter SET grade = ?, state = ? WHERE id = ?";

inline constexpr std::string_view MARK_ENCOUNTER_ACTION =
    "UPDATE guard_encounter SET last_action_at = ? WHERE id = ?";

inline constexpr std::string_view INSERT_ENCOUNTER_OUTBOX =
    "INSERT OR IGNORE INTO guard_encounter_outbox (event_id, payload, "
    "created_at, updated_at) VALUES (?, ?, ?, ?)";

inline constexpr std::string_view PENDING_ENCOUNTER_OUTBOX =
    "SELECT event_id, payload FROM guard_encounter_outbox WHERE status = "
    "'pending' ORDER BY created_at ASC LIMIT 100";

inline constexpr std::string_view MARK_ENCOUNTER_SENT =
    "UPDATE guard_encounter_outbox SET status = 'sent', updated_at = ? "
    "WHERE event_id = ?";

inline constexpr std::string_view RECORD_ENCOUNTER_ATTEMPT =
    "UPDATE guard_encounter_outbox SET attempts = attempts + 1, updated_at = ? "
    "WHERE event_id = ?";

inline constexpr std::string_view CLOSE_STALE_TRANSITIONS =
    "INSERT INTO guard_encounter_transition (encounter_id, from_state, "
    "to_state, reason, revision, occurred_at) "
    "SELECT id, state, 'closed', 'stale', revision + 1, ? FROM guard_encounter "
    "WHERE state != 'closed' AND last_seen < ?";

inline constexpr std::string_view CLOSE_STALE_ENCOUNTERS =
    "UPDATE guard_encounter SET state = 'closed', revision = revision + 1 "
    "WHERE state != 'closed' AND last_seen < ? "
    "RETURNING id, person_id, grade, best_camera_id, first_seen, last_seen";

inline constexpr std::string_view CLOSE_PERSON_TRANSITIONS =
    "INSERT INTO guard_encounter_transition (encounter_id, from_state, "
    "to_state, reason, revision, occurred_at) "
    "SELECT id, state, 'closed', 'known_resident', revision + 1, ? "
    "FROM guard_encounter WHERE person_id = ? AND state != 'closed'";

inline constexpr std::string_view CLOSE_ENCOUNTERS_FOR_PERSON =
    "UPDATE guard_encounter SET state = 'closed', revision = revision + 1, "
    "last_seen = ? WHERE person_id = ? AND state != 'closed'";

inline constexpr std::string_view ENCOUNTER_LAST_ACTION =
    "SELECT last_action_at FROM guard_encounter WHERE id = ?";

inline constexpr std::string_view INSERT_ASSESSMENT =
    "INSERT OR IGNORE INTO guard_assessment (incident_id, camera_id, event_id, "
    "mode, caption, threat, veto, tags, summary, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view SELECT_ENCOUNTER_STATE =
    "SELECT state, revision FROM guard_encounter WHERE id = ?";

inline constexpr std::string_view UPDATE_ENCOUNTER_STATE_REVISION =
    "UPDATE guard_encounter SET grade = ?, state = ?, "
    "revision = revision + 1 WHERE id = ?";

inline constexpr std::string_view INSERT_TRANSITION =
    "INSERT INTO guard_encounter_transition (encounter_id, from_state, "
    "to_state, reason, revision, occurred_at) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view INSERT_INBOX =
    "INSERT OR IGNORE INTO guard_observation_inbox (event_id, camera_id, "
    "observation_id, received_at, status, attempts, payload) VALUES (?, ?, ?, "
    "?, 'processing', 1, ?)";

inline constexpr std::string_view SELECT_INBOX =
    "SELECT status, attempts, stage, incident_id, encounter_id, danger, "
    "checkpoint, payload, retry_at, local_retries "
    "FROM guard_observation_inbox WHERE event_id = ?";

inline constexpr std::string_view ADVANCE_INBOX =
    "UPDATE guard_observation_inbox SET stage = ?, incident_id = ?, "
    "encounter_id = ?, danger = ?, checkpoint = ?, updated_at = ? "
    "WHERE event_id = ?";

// Broker deliveries drive the dead-letter threshold; local retries never
// increment this count.
inline constexpr std::string_view REVIVE_INBOX =
    "UPDATE guard_observation_inbox SET status = 'processing', "
    "attempts = MAX(attempts, ?), payload = CASE WHEN ? = '' THEN payload ELSE ? END "
    "WHERE event_id = ? AND status = 'processing'";

// A broker redelivery while the local retry owns the row records the delivery
// without disturbing the scheduled retry or its local-retry count.
inline constexpr std::string_view TOUCH_SCHEDULED_INBOX =
    "UPDATE guard_observation_inbox SET attempts = MAX(attempts, ?), "
    "updated_at = ? WHERE event_id = ? AND status = 'processing'";

// Durable local retry: the payload and the due time are persisted before the
// broker message is acknowledged, so a crash cannot strand the observation.
inline constexpr std::string_view SET_INBOX_RETRY =
    "UPDATE guard_observation_inbox SET retry_at = ?, "
    "local_retries = local_retries + 1, payload = ?, updated_at = ? "
    "WHERE event_id = ? AND status = 'processing'";

inline constexpr std::string_view DUE_INBOX =
    "SELECT event_id, payload FROM guard_observation_inbox "
    "WHERE status = 'processing' AND retry_at > 0 AND retry_at <= ? "
    "ORDER BY retry_at ASC LIMIT 100";

// Leases a due row so a crash mid-retry is recovered by the next reconcile.
inline constexpr std::string_view CLAIM_INBOX_RETRY =
    "UPDATE guard_observation_inbox SET retry_at = ? "
    "WHERE event_id = ? AND status = 'processing' AND retry_at > 0 "
    "AND retry_at <= ?";

inline constexpr std::string_view DEAD_LETTER_INBOX =
    "UPDATE guard_observation_inbox SET status = 'dead_lettered', "
    "updated_at = ? WHERE event_id = ?";

inline constexpr std::string_view COMPLETE_INBOX =
    "UPDATE guard_observation_inbox SET status = 'completed', completed_at = ? "
    "WHERE event_id = ?";

inline constexpr std::string_view INSERT_OUTBOX =
    "INSERT OR IGNORE INTO guard_action_outbox (command_id, encounter_id, "
    "incident_id, camera_id, person_id, kind, status, detail, payload, "
    "attempts, created_at, updated_at) "
    "VALUES (?, ?, ?, ?, ?, ?, 'pending', '', ?, 0, ?, ?)";

inline constexpr std::string_view SELECT_OUTBOX =
    "SELECT status, detail, response, payload, attempts, next_attempt_at "
    "FROM guard_action_outbox WHERE command_id = ?";

// Compare-and-set: only fills an empty payload or accepts an identical one.
inline constexpr std::string_view SET_OUTBOX_PAYLOAD =
    "UPDATE guard_action_outbox SET payload = ?, updated_at = ? "
    "WHERE command_id = ? AND (payload = '' OR payload = ?)";

inline constexpr std::string_view UPDATE_OUTBOX =
    "UPDATE guard_action_outbox SET status = ?, detail = ?, response = ?, "
    "attempts = attempts + 1, next_attempt_at = ?, updated_at = ? "
    "WHERE command_id = ?";

inline constexpr std::string_view INSERT_DEAD_LETTER =
    "INSERT OR IGNORE INTO guard_dead_letter (event_id, payload, reason, "
    "attempts, created_at) VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view INSERT_EVIDENCE =
    "INSERT OR IGNORE INTO guard_evidence (incident_id, encounter_id, "
    "camera_id, "
    "object_key, content_type, retention_class, created_at, expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view EXPIRED_EVIDENCE =
    "SELECT id, object_key FROM guard_evidence "
    "WHERE deleted_at = 0 AND expires_at > 0 AND expires_at <= ? LIMIT 200";

inline constexpr std::string_view MARK_EVIDENCE_DELETED =
    "UPDATE guard_evidence SET deleted_at = ? WHERE id = ?";
} // namespace guard_query

struct GuardIncidentInput
{
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::string danger;
  std::string severity;
  int64_t personId{0};
  std::string identity;
  std::string eventId;
  std::string eventJson;
  int64_t createdAt{0};
};

struct GuardActionInput
{
  int64_t incidentId{0};
  int64_t encounterId{0};
  int64_t cameraId{0};
  int64_t personId{0};
  std::string commandId;
  std::string kind;
  std::string status;
  std::string detail;
  int64_t createdAt{0};
};

struct GuardCanActInput
{
  int64_t cameraId{0};
  int64_t personId{0};
  int64_t cooldownS{0};
  int64_t now{0};
  int64_t maxPerHour{0};
};

struct GuardStateInput
{
  std::string key;
  std::string value;
  int64_t updatedAt{0};
};

struct GuardGuestInput
{
  std::string description;
  int64_t cameraId{0};
  int64_t personId{0};
  int64_t hostUserId{0};
  bool oneTime{false};
  int64_t validFrom{0};
  int64_t validUntil{0};
};

struct GuardGuest
{
  int64_t id{0};
  std::string description;
  int64_t cameraId{0};
  int64_t personId{0};
  int64_t hostUserId{0};
  bool oneTime{false};
  int64_t validFrom{0};
  int64_t validUntil{0};
};

struct GuardAssessmentRowInput
{
  int64_t incidentId{0};
  int64_t cameraId{0};
  std::string eventId;
  std::string mode;
  std::string caption;
  std::string threat;
  bool veto{false};
  std::string tags;
  std::string summary;
  int64_t createdAt{0};
};

struct GuardEncounter
{
  int64_t id{0};
  int64_t personId{0};
  std::string signature;
  EncounterState state{EncounterState::Observing};
  std::string grade;
  int checks{0};
  int64_t bestCameraId{0};
  double bestScore{0.0};
  int dialogueTurns{0};
  std::string dialogueGoal;
  int64_t listeningUntil{0};
  std::string lastLine;
  std::string lastHeard;
  int64_t lastHeardAt{0};
  int64_t firstSeen{0};
  int64_t lastSeen{0};
};

// Stable event id and immutable payload for a closed encounter.
inline std::string encounterClosedEventId(const GuardEncounter& encounter,
                                          int64_t at)
{
  return "encounter:" + std::to_string(encounter.id) + ":" + encounter.grade +
         ":" + std::to_string(at);
}

inline std::string encounterClosedPayload(const GuardEncounter& encounter,
                                          int64_t at)
{
  const int64_t duration = encounter.lastSeen > encounter.firstSeen
                               ? encounter.lastSeen - encounter.firstSeen
                               : 0;
  Json::Value payload(Json::objectValue);
  payload["eventId"] = encounterClosedEventId(encounter, at);
  payload["encounterId"] = Json::Int64(encounter.id);
  payload["cameraId"] = Json::Int64(encounter.bestCameraId);
  payload["personId"] = Json::Int64(encounter.personId);
  payload["grade"] = encounter.grade;
  payload["durationS"] = Json::Int64(duration);
  payload["closedAt"] = Json::Int64(at);
  return json_util::toString(payload);
}

struct EncounterOutboxInput
{
  std::string eventId;
  std::string payload;
  int64_t at{0};
};

struct EncounterOutboxRow
{
  std::string eventId;
  std::string payload;
};

struct GuardEncounterCreateInput
{
  int64_t personId{0};
  std::string signature;
  int64_t bestCameraId{0};
  double bestScore{0.0};
  int64_t at{0};
};

struct GuardEncounterTouchInput
{
  int64_t id{0};
  int64_t bestCameraId{0};
  double bestScore{0.0};
  int64_t lastSeen{0};
};

struct GuardEncounterActionInput
{
  int64_t id{0};
  int64_t now{0};
  int64_t cooldownS{0};
  int64_t maxPerHour{0};
};

struct GuardStaleEncountersInput
{
  int64_t olderThan{0};
  int64_t closedAt{0};
};

struct GuardInboxInput
{
  std::string eventId;
  int64_t cameraId{0};
  std::string observationId;
  int64_t receivedAt{0};
  std::string payload;
  // Broker JetStream delivery count; local retries leave it unchanged.
  int delivered{0};
};

struct GuardInboxRetryInput
{
  std::string eventId;
  std::string payload;
  int64_t retryAt{0};
  int64_t at{0};
};

struct GuardInboxDueRow
{
  std::string eventId;
  std::string payload;
};

struct GuardInboxRetryClaimInput
{
  std::string eventId;
  int64_t leaseUntil{0};
  int64_t now{0};
};

enum class ObservationClaimKind : uint8_t
{
  New = 0,
  Retry,
  Completed,
  Scheduled
};

struct ObservationClaim
{
  ObservationClaimKind kind{ObservationClaimKind::New};
  int attempts{0};
  int localRetries{0};
  int stage{0};
  int64_t incidentId{0};
  int64_t encounterId{0};
  std::string danger;
  std::string checkpoint;
  std::string payload;
  int64_t retryAt{0};
};

struct GuardObservationAdvanceInput
{
  std::string eventId;
  int stage{0};
  int64_t incidentId{0};
  int64_t encounterId{0};
  std::string danger;
  std::string checkpoint;
  int64_t at{0};
};

struct GuardIncidentPhaseInput
{
  GuardIncidentInput incident;
  // One-time guest consumed atomically with the incident and the checkpoint.
  int64_t consumeGuestId{0};
  int64_t consumeGuestAt{0};
  GuardObservationAdvanceInput advance;
};

struct GuardIncidentPhaseResult
{
  int64_t incidentId{0};
  bool committed{false};
};

struct GuardEncounterPhaseInput
{
  // Precomputed policy decision: create a new encounter or touch a matched one.
  bool create{false};
  int64_t encounterIdToTouch{0};
  GuardEncounterCreateInput createInput;
  GuardEncounterTouchInput touchInput;
  bool closeForPerson{false};
  int64_t closePersonId{0};
  int64_t closeAt{0};
  GuardObservationAdvanceInput advance;
};

struct GuardEncounterPhaseResult
{
  int64_t encounterId{0};
  int encounterChecks{0};
  std::vector<GuardEncounter> closed;
  bool committed{false};
};

struct GuardIntent
{
  GuardIntentStatus status{GuardIntentStatus::Pending};
  std::string detail;
  std::string response;
  std::string payload;
  int attempts{0};
  int64_t nextAttemptAt{0};
};

struct GuardDeadLetterInput
{
  std::string eventId;
  std::string payload;
  std::string reason;
  int attempts{0};
  int64_t at{0};
};

struct GuardDialogueInput
{
  int64_t encounterId{0};
  std::string turnKey;
  std::string goal;
  int64_t listeningUntil{0};
  std::string lastLine;
  std::string lastHeard;
  int64_t at{0};
};

// Goal/window transition that does not consume a dialogue turn.
struct GuardDialogueGoalInput
{
  int64_t encounterId{0};
  std::string goal;
  int64_t listeningUntil{0};
  std::string lastLine;
  std::string lastHeard;
};

struct GuardParkInput
{
  GuardDeadLetterInput letter;
  int64_t at{0};
};

struct GuardTransitionInput
{
  int64_t encounterId{0};
  EncounterState toState{EncounterState::Observing};
  std::string reason;
  std::string grade;
  int64_t at{0};
};

struct GuardOutboxInput
{
  std::string commandId;
  int64_t encounterId{0};
  int64_t incidentId{0};
  int64_t cameraId{0};
  int64_t personId{0};
  std::string kind;
  std::string payload;
  int64_t at{0};
};

struct GuardOutboxPayloadInput
{
  std::string commandId;
  std::string payload;
  int64_t at{0};
};

struct GuardOutboxUpdateInput
{
  std::string commandId;
  GuardIntentStatus status{GuardIntentStatus::Pending};
  std::string detail;
  std::string response;
  int64_t nextAttemptAt{0};
  int64_t at{0};
};

struct GuardEvidenceInput
{
  int64_t incidentId{0};
  int64_t encounterId{0};
  int64_t cameraId{0};
  std::string objectKey;
  std::string contentType;
  std::string retentionClass;
  int64_t createdAt{0};
  int64_t expiresAt{0};
};

struct GuardEvidenceRow
{
  int64_t id{0};
  std::string objectKey;
};
