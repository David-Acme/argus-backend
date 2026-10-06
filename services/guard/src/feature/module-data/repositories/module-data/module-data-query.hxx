#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace module_data_query
{

inline constexpr std::string_view SUMMARY =
    "SELECT "
    "(SELECT COUNT(*) FROM guard_environment WHERE is_default = 0) AS environments, "
    "(SELECT COUNT(*) FROM guard_encounter) AS episodes, "
    "(SELECT COUNT(*) FROM guard_incident) AS incidents, "
    "(SELECT COUNT(*) FROM guard_decision_journal) AS decisions, "
    "(SELECT COUNT(*) FROM guard_expected_guest WHERE deleted_at IS NULL) AS guests, "
    "(SELECT COUNT(*) FROM guard_evidence WHERE deleted_at = 0) AS evidence, "
    "(SELECT COALESCE(SUM(?1 + length(camera_name) + length(identity) + length(event_json)), 0) "
    "FROM guard_incident) + "
    "(SELECT COALESCE(SUM(?1 + length(detail)), 0) FROM guard_action) + "
    "(SELECT COALESCE(SUM(?1 + length(caption) + length(summary) + length(tags)), 0) FROM guard_assessment) + "
    "(SELECT COALESCE(SUM(?1 + length(signature) + length(last_line) + length(last_heard) + length(reasons)), 0) "
    "FROM guard_encounter) + "
    "(SELECT COUNT(*) * ?1 FROM guard_encounter_transition) + "
    "(SELECT COALESCE(SUM(?1 + length(checkpoint) + length(payload)), 0) FROM guard_observation_inbox) + "
    "(SELECT COALESCE(SUM(?1 + length(payload)), 0) FROM guard_dead_letter) + "
    "(SELECT COALESCE(SUM(?1 + length(payload) + length(response)), 0) FROM guard_action_outbox) + "
    "(SELECT COALESCE(SUM(?1 + length(payload)), 0) FROM guard_encounter_outbox) + "
    "(SELECT COALESCE(SUM(?1 + length(object_key)), 0) FROM guard_evidence) + "
    "(SELECT COUNT(*) * ?1 FROM guard_hourly_baseline) + "
    "(SELECT COALESCE(SUM(?1 + length(signature)), 0) FROM guard_signature_visit) + "
    "(SELECT COALESCE(SUM(?1 + length(belief_signals) + length(reasons)), 0) FROM guard_decision_journal) + "
    "(SELECT COALESCE(SUM(?1 + length(description)), 0) FROM guard_expected_guest) + "
    "(SELECT COUNT(*) * ?1 FROM guard_camera_context) + "
    "(SELECT COALESCE(SUM(?1 + length(name)), 0) FROM guard_environment WHERE is_default = 0) AS bytes";

inline constexpr std::array<std::string_view, 22> PURGE{
    "UPDATE guard_evidence SET expires_at = 1 WHERE deleted_at = 0",
    "DELETE FROM guard_evidence WHERE deleted_at > 0",
    "DELETE FROM guard_incident",
    "DELETE FROM guard_action",
    "DELETE FROM guard_assessment",
    "DELETE FROM guard_encounter_transition",
    "DELETE FROM guard_encounter",
    "DELETE FROM guard_observation_inbox",
    "DELETE FROM guard_dead_letter",
    "DELETE FROM guard_action_outbox",
    "DELETE FROM guard_encounter_outbox",
    "DELETE FROM guard_hourly_baseline",
    "DELETE FROM guard_signature_visit",
    "DELETE FROM guard_decision_journal",
    "DELETE FROM guard_expected_guest",
    "DELETE FROM guard_camera_context",
    R"(DELETE FROM guard_state WHERE key LIKE 'tamper\_onset\_%' ESCAPE '\')",
    "DELETE FROM guard_presence WHERE environment_id NOT IN "
    "(SELECT id FROM guard_environment WHERE is_default = 1)",
    "DELETE FROM guard_response_recipient WHERE environment_id NOT IN "
    "(SELECT id FROM guard_environment WHERE is_default = 1)",
    "DELETE FROM guard_response_contact WHERE environment_id NOT IN "
    "(SELECT id FROM guard_environment WHERE is_default = 1)",
    "DELETE FROM guard_response_setting WHERE environment_id NOT IN "
    "(SELECT id FROM guard_environment WHERE is_default = 1)",
    "DELETE FROM guard_environment WHERE is_default = 0"};

inline constexpr std::string_view PENDING_EVIDENCE =
    "SELECT id, object_key FROM guard_evidence WHERE deleted_at = 0 ORDER BY id LIMIT ?";

inline constexpr std::string_view COUNT_PENDING_EVIDENCE =
    "SELECT COUNT(*) AS total FROM guard_evidence WHERE deleted_at = 0";

inline constexpr std::string_view DELETE_EVIDENCE = "DELETE FROM guard_evidence WHERE id = ?";

inline constexpr std::int64_t kRowOverheadBytes = 64;
inline constexpr std::int64_t kEvidenceBatch = 200;

}

struct PendingEvidence
{
  std::int64_t id{0};
  std::string objectKey;
};
