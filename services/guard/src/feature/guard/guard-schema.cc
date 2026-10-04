#include "guard-schema.hxx"

#include <drogon/drogon.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <trantor/utils/Logger.h>
#include <vector>

namespace
{

bool exec(const std::string& statement)
{
  try {
    DbService::client()->execSqlSync(statement);
    return true;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Guard schema migration statement failed: " << e.what();
    return false;
  }
}

bool tableExists(const std::string& name)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM sqlite_master WHERE type = 'table' "
      "AND name = ?",
      name);
  return !rows.empty() && rows.front()["total"].as<int>() > 0;
}

std::string tableSql(const std::string& name)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?", name);
  return rows.empty() ? std::string{} : rows.front()["sql"].as<std::string>();
}

bool columnExists(const std::string& table, const std::string& column)
{
  const auto rows =
      DbService::client()
          ->execSqlSync("SELECT COUNT(*) AS total FROM pragma_table_info('" +
                            table + "') WHERE name = ?",
                        column);
  return !rows.empty() && rows.front()["total"].as<int>() > 0;
}

std::vector<std::string> schemaStatements(const std::string& schemaPath,
                                          const std::string& needle)
{
  std::ifstream file(schemaPath);
  std::vector<std::string> statements;
  std::string current;
  std::string line;
  while (std::getline(file, line)) {
    const auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    const auto end = line.find_last_not_of(" \t\r\n");
    const std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.rfind("--", 0) == 0)
      continue;
    current += trimmed + "\n";
    if (trimmed.back() != ';')
      continue;
    if (current.find(needle) != std::string::npos)
      statements.push_back(current);
    current.clear();
  }
  return statements;
}

bool encounterTableIsCurrent()
{
  if (!tableExists("guard_encounter"))
    return true;
  const std::string sql = tableSql("guard_encounter");
  return sql.find("verifying") != std::string::npos ||
         sql.find("challenging") != std::string::npos;
}

bool rebuildEncounterTable(const std::string& schemaPath)
{
  const auto statements = schemaStatements(schemaPath, "guard_encounter");
  if (statements.empty()) {
    LOG_WARN << "Guard schema migration: guard_encounter DDL not found";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok =
      exec("DROP INDEX IF EXISTS idx_guard_encounter_last_seen") &&
      exec("DROP INDEX IF EXISTS idx_guard_encounter_person") &&
      exec("ALTER TABLE guard_encounter RENAME TO guard_encounter_legacy");
  for (const auto& statement : statements)
    ok = ok && exec(statement);
  ok = ok &&
       exec("INSERT INTO guard_encounter (id, person_id, signature, state, "
            "grade, checks, revision, best_camera_id, best_score, first_seen, "
            "last_seen, last_action_at) SELECT id, person_id, signature, "
            "CASE state WHEN 'assessing' THEN 'verifying' "
            "WHEN 'observed' THEN 'observing' ELSE state END, grade, checks, "
            "revision, best_camera_id, best_score, first_seen, last_seen, "
            "last_action_at FROM guard_encounter_legacy") &&
       exec("DROP TABLE guard_encounter_legacy");
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool migrateInboxColumns()
{
  if (!tableExists("guard_observation_inbox"))
    return true;
  if (!columnExists("guard_observation_inbox", "status") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN status TEXT "
            "NOT NULL DEFAULT 'processing'"))
    return false;
  if (!columnExists("guard_observation_inbox", "attempts") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN attempts INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "completed_at") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN completed_at "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "stage") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN stage INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "incident_id") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN incident_id "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "encounter_id") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN encounter_id "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "danger") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN danger TEXT "
            "NOT NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_observation_inbox", "checkpoint") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN checkpoint TEXT "
            "NOT NULL DEFAULT '{}'"))
    return false;
  if (!columnExists("guard_observation_inbox", "updated_at") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN updated_at "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "payload") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN payload TEXT NOT "
            "NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_observation_inbox", "retry_at") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN retry_at INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_observation_inbox", "local_retries") &&
      !exec("ALTER TABLE guard_observation_inbox ADD COLUMN local_retries "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool inboxCheckIsCurrent()
{
  if (!tableExists("guard_observation_inbox"))
    return true;
  const std::string sql = tableSql("guard_observation_inbox");
  return sql.find("dead_lettered") != std::string::npos;
}

bool rebuildInboxTable(const std::string& schemaPath)
{
  const auto statements =
      schemaStatements(schemaPath, "guard_observation_inbox");
  if (statements.empty()) {
    LOG_WARN << "Guard schema migration: inbox DDL not found";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok = exec("ALTER TABLE guard_observation_inbox RENAME TO "
                 "guard_observation_inbox_legacy");
  for (const auto& statement : statements)
    ok = ok && exec(statement);
  ok = ok &&
       exec("INSERT INTO guard_observation_inbox (event_id, camera_id, "
            "observation_id, status, attempts, stage, incident_id, "
            "encounter_id, danger, checkpoint, received_at, updated_at, "
            "completed_at) SELECT event_id, camera_id, observation_id, status, "
            "attempts, stage, incident_id, encounter_id, danger, checkpoint, "
            "received_at, updated_at, completed_at "
            "FROM guard_observation_inbox_legacy") &&
       exec("DROP TABLE guard_observation_inbox_legacy");
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool migrateIncidentColumns()
{
  if (!tableExists("guard_incident"))
    return true;
  if (!columnExists("guard_incident", "event_id") &&
      !exec("ALTER TABLE guard_incident ADD COLUMN event_id TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  return exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_incident_event "
              "ON guard_incident (event_id) WHERE event_id != ''");
}

bool migrateActionColumns()
{
  if (!tableExists("guard_action"))
    return true;
  if (!columnExists("guard_action", "command_id") &&
      !exec("ALTER TABLE guard_action ADD COLUMN command_id TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  return exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_action_command "
              "ON guard_action (command_id) WHERE command_id != ''");
}

bool actionTableIsCurrent()
{
  if (!tableExists("guard_action"))
    return true;
  const std::string sql = tableSql("guard_action");
  return sql.find("greet_listen") != std::string::npos;
}

bool rebuildActionTable(const std::string& schemaPath)
{
  const auto statements = schemaStatements(schemaPath, "guard_action");
  if (statements.empty()) {
    LOG_WARN << "Guard schema migration: guard_action DDL not found";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok =
      exec("DROP INDEX IF EXISTS idx_guard_action_camera_created") &&
      exec("DROP INDEX IF EXISTS idx_guard_action_encounter_created") &&
      exec("DROP INDEX IF EXISTS idx_guard_action_command") &&
      exec("ALTER TABLE guard_action RENAME TO guard_action_legacy");
  for (const auto& statement : statements)
    ok = ok && exec(statement);
  ok = ok &&
       exec("INSERT INTO guard_action (id, incident_id, encounter_id, "
            "camera_id, person_id, command_id, kind, status, detail, "
            "created_at) SELECT id, incident_id, encounter_id, camera_id, "
            "person_id, command_id, CASE "
            "WHEN kind IN ('greet', 'greet_listen', 'greet_reply', "
            "'announce', 'alarm', 'siren_arm', 'siren_disarm', 'notify') "
            "THEN kind "
            "WHEN kind LIKE 'agent\\_%' ESCAPE '\\' THEN kind "
            "ELSE 'agent_legacy' END, status, detail, created_at FROM "
            "guard_action_legacy") &&
       exec("DROP TABLE guard_action_legacy");
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool migrateAssessmentColumns()
{
  if (!tableExists("guard_assessment"))
    return true;
  if (!columnExists("guard_assessment", "event_id") &&
      !exec("ALTER TABLE guard_assessment ADD COLUMN event_id TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  return exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_assessment_event "
              "ON guard_assessment (event_id) WHERE event_id != ''");
}

bool migrateEncounterDialogueColumns()
{
  if (!tableExists("guard_encounter"))
    return true;
  if (!columnExists("guard_encounter", "dialogue_turns") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN dialogue_turns INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "dialogue_turn_key") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN dialogue_turn_key TEXT NOT "
            "NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "dialogue_goal") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN dialogue_goal TEXT NOT "
            "NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "listening_until") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN listening_until INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "last_line") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN last_line TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "last_heard") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN last_heard TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "last_heard_at") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN last_heard_at INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool migrateEncounterNotifyColumns()
{
  if (!tableExists("guard_encounter"))
    return true;
  if (!columnExists("guard_encounter", "notify_command_id") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN notify_command_id TEXT "
            "NOT NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "notify_count") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN notify_count INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "notify_highest_rank") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN notify_highest_rank "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool migrateEncounterEpisodeColumns()
{
  if (!tableExists("guard_encounter"))
    return true;
  if (!columnExists("guard_encounter", "subject") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN subject TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  if (!columnExists("guard_encounter", "people") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN people INTEGER NOT NULL "
            "DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "reasons") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN reasons TEXT NOT NULL "
            "DEFAULT '[]'"))
    return false;
  if (!columnExists("guard_encounter", "reasons_rank") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN reasons_rank INTEGER NOT "
            "NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "group_id") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN group_id INTEGER NOT NULL "
            "DEFAULT 0"))
    return false;
  if (!columnExists("guard_encounter", "review_label") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN review_label TEXT NOT NULL "
            "DEFAULT '' CHECK (review_label IN ('', 'useful', 'false_alarm', "
            "'not_now'))"))
    return false;
  if (!columnExists("guard_encounter", "reviewed_at") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN reviewed_at INTEGER NOT "
            "NULL DEFAULT 0"))
    return false;
  return true;
}

bool migrateJournalReasonsColumn()
{
  if (!tableExists("guard_decision_journal"))
    return true;
  if (!columnExists("guard_decision_journal", "reasons") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN reasons TEXT NOT "
            "NULL DEFAULT '[]'"))
    return false;
  return true;
}

bool migrateJournalDispatchColumn()
{
  if (!tableExists("guard_decision_journal"))
    return true;
  if (!columnExists("guard_decision_journal", "dispatch_attempts") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN dispatch_attempts "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool migrateJournalRound11Columns()
{
  if (!tableExists("guard_decision_journal"))
    return true;
  if (!columnExists("guard_decision_journal", "suppressed_kinds") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN suppressed_kinds "
            "TEXT NOT NULL DEFAULT '[]'"))
    return false;
  if (!columnExists("guard_decision_journal", "novelty_score") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN novelty_score REAL "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_decision_journal", "repeat_visits") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN repeat_visits "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_decision_journal", "quiet_hold") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN quiet_hold INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_decision_journal", "budget_hold") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN budget_hold INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_decision_journal", "assess_ms") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN assess_ms INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_decision_journal", "feedback_label") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN feedback_label TEXT "
            "NOT NULL DEFAULT ''"))
    return false;
  if (!columnExists("guard_decision_journal", "feedback_at") &&
      !exec("ALTER TABLE guard_decision_journal ADD COLUMN feedback_at INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool migrateOutboxColumns()
{
  if (!tableExists("guard_action_outbox"))
    return true;
  if (!columnExists("guard_action_outbox", "response") &&
      !exec("ALTER TABLE guard_action_outbox ADD COLUMN response TEXT NOT NULL "
            "DEFAULT '{}'"))
    return false;
  if (!columnExists("guard_action_outbox", "payload") &&
      !exec("ALTER TABLE guard_action_outbox ADD COLUMN payload TEXT NOT NULL "
            "DEFAULT ''"))
    return false;
  if (!columnExists("guard_action_outbox", "next_attempt_at") &&
      !exec("ALTER TABLE guard_action_outbox ADD COLUMN next_attempt_at "
            "INTEGER NOT NULL DEFAULT 0"))
    return false;
  return true;
}

bool outboxStatusIsCurrent()
{
  if (!tableExists("guard_action_outbox"))
    return true;
  const std::string sql = tableSql("guard_action_outbox");
  return sql.find("retryable_failed") != std::string::npos &&
         sql.find("duplicate_succeeded") != std::string::npos;
}

bool rebuildActionOutboxTable(const std::string& schemaPath)
{
  const auto statements = schemaStatements(schemaPath, "guard_action_outbox");
  if (statements.empty()) {
    LOG_WARN << "Guard schema migration: action outbox DDL not found";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok = exec("DROP INDEX IF EXISTS idx_guard_outbox_status") &&
            exec("ALTER TABLE guard_action_outbox RENAME TO "
                 "guard_action_outbox_legacy");
  for (const auto& statement : statements)
    ok = ok && exec(statement);
  ok = ok &&
       exec("INSERT INTO guard_action_outbox (command_id, encounter_id, "
            "incident_id, camera_id, person_id, kind, status, detail, "
            "response, payload, attempts, next_attempt_at, created_at, "
            "updated_at) SELECT command_id, encounter_id, incident_id, "
            "camera_id, person_id, kind, CASE status "
            "WHEN 'sent' THEN 'succeeded' WHEN 'denied' THEN 'rejected' "
            "WHEN 'failed' THEN 'retryable_failed' WHEN 'unknown' THEN "
            "'rejected' ELSE status END, detail, response, payload, attempts, "
            "0, created_at, updated_at FROM guard_action_outbox_legacy") &&
       exec("DROP TABLE guard_action_outbox_legacy");
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool ensureBaselineTables()
{
  return exec("CREATE TABLE IF NOT EXISTS guard_hourly_baseline ("
              "camera_id INTEGER NOT NULL, "
              "dow_hour INTEGER NOT NULL CHECK (dow_hour >= 0 AND "
              "dow_hour < 168), "
              "events_ema REAL NOT NULL DEFAULT 0, "
              "updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
              "PRIMARY KEY (camera_id, dow_hour))") &&
         exec("CREATE TABLE IF NOT EXISTS guard_signature_visit ("
              "signature TEXT NOT NULL PRIMARY KEY, "
              "visits INTEGER NOT NULL DEFAULT 0, "
              "first_seen INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
              "last_seen INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
}

bool ensureDeadLetterTable()
{
  return exec("CREATE TABLE IF NOT EXISTS guard_dead_letter ("
              "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
              "event_id TEXT NOT NULL DEFAULT '', payload TEXT NOT NULL "
              "DEFAULT '', "
              "reason TEXT NOT NULL DEFAULT '', attempts INTEGER NOT NULL "
              "DEFAULT 0, "
              "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))") &&
         exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_dead_letter_event "
              "ON guard_dead_letter (event_id) WHERE event_id != ''");
}

bool ensureEncounterOutboxTable()
{
  return exec("CREATE TABLE IF NOT EXISTS guard_encounter_outbox ("
              "event_id TEXT NOT NULL PRIMARY KEY, payload TEXT NOT NULL "
              "DEFAULT '{}', "
              "status TEXT NOT NULL DEFAULT 'pending', "
              "attempts INTEGER NOT NULL DEFAULT 0, "
              "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
              "updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))") &&
         exec("CREATE INDEX IF NOT EXISTS idx_guard_encounter_outbox_status "
              "ON guard_encounter_outbox (status, created_at ASC)");
}

bool ensureEvidenceIndex()
{
  if (!tableExists("guard_evidence"))
    return true;
  return exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_evidence_object "
              "ON guard_evidence (object_key) WHERE object_key != ''");
}

bool journalTableIsCurrent()
{
  if (!tableExists("guard_decision_journal"))
    return true;
  const std::string sql = tableSql("guard_decision_journal");
  return sql.find("suppressed_kinds") != std::string::npos &&
         sql.find("'held'") != std::string::npos;
}

bool rebuildJournalTable(const std::string& schemaPath)
{
  const auto statements = schemaStatements(schemaPath, "guard_decision_journal");
  if (statements.empty()) {
    LOG_WARN << "Guard schema migration: guard_decision_journal DDL not found";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok =
      exec("DROP INDEX IF EXISTS idx_guard_decision_journal_encounter") &&
      exec("DROP INDEX IF EXISTS idx_guard_decision_journal_cursor") &&
      exec("DROP INDEX IF EXISTS idx_guard_decision_journal_camera_time") &&
      exec("ALTER TABLE guard_decision_journal RENAME TO "
           "guard_decision_journal_legacy");
  for (const auto& statement : statements)
    ok = ok && exec(statement);
  ok = ok &&
       exec("INSERT INTO guard_decision_journal (event_id, encounter_id, "
            "incident_id, camera_id, observation_id, severity, severity_rank, "
            "hard_floor, belief_score, belief_signals, belief_threshold, "
            "legacy_would_notify, belief_would_notify, did_notify, "
            "decision_mode, suppression_reason, suppressed_kinds, reasons, "
            "dispatch_attempts, novelty_score, repeat_visits, quiet_hold, "
            "budget_hold, assess_ms, feedback_label, feedback_at, created_at) "
            "SELECT event_id, encounter_id, incident_id, camera_id, "
            "observation_id, severity, severity_rank, hard_floor, belief_score, "
            "belief_signals, belief_threshold, legacy_would_notify, "
            "belief_would_notify, did_notify, decision_mode, "
            "suppression_reason, suppressed_kinds, reasons, dispatch_attempts, "
            "novelty_score, repeat_visits, quiet_hold, budget_hold, assess_ms, "
            "feedback_label, feedback_at, created_at FROM "
            "guard_decision_journal_legacy") &&
       exec("DROP TABLE guard_decision_journal_legacy");
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool addEnvironmentColumn(const std::string& table)
{
  if (!tableExists(table) || columnExists(table, "environment_id"))
    return true;
  return exec("ALTER TABLE " + table +
              " ADD COLUMN environment_id INTEGER NOT NULL DEFAULT 0");
}

bool migrateEnvironmentColumns()
{
  return addEnvironmentColumn("guard_camera_context") &&
         addEnvironmentColumn("guard_encounter") &&
         addEnvironmentColumn("guard_decision_journal") &&
         addEnvironmentColumn("guard_expected_guest");
}

struct SeedRow
{
  std::string kind;
  int scheduleEnabled{0};
  std::string asleep;
  std::string open;
  std::string staffed;
  std::string closedMode;
  int digestHour{21};
};

SeedRow seedRow(const GuardEnvironment& seed)
{
  SeedRow row{.kind = environmentKindToString(seed.kind),
              .scheduleEnabled = seed.scheduleEnabled ? 1 : 0,
              .asleep = seed.asleep,
              .open = seed.open,
              .staffed = seed.staffed,
              .closedMode = seed.closedMode == GuardMode::Armed ? "armed" : "away",
              .digestHour = seed.digestHour};
  if (!tableExists("guard_site"))
    return row;
  const auto rows = DbService::client()->execSqlSync(
      "SELECT profile, schedule_enabled, asleep_hours, open_hours, "
      "staffed_hours, closed_mode, digest_hour FROM guard_site WHERE id = 1");
  if (rows.empty())
    return row;
  const auto& site = rows.front();
  row.kind = environmentKindFromString(site["profile"].as<std::string>())
                 .has_value()
                 ? site["profile"].as<std::string>()
                 : row.kind;
  row.scheduleEnabled = site["schedule_enabled"].as<int>() != 0 ? 1 : 0;
  row.asleep = site["asleep_hours"].as<std::string>();
  row.open = site["open_hours"].as<std::string>();
  row.staffed = site["staffed_hours"].as<std::string>();
  row.closedMode = site["closed_mode"].as<std::string>() == "armed" ? "armed"
                                                                     : "away";
  row.digestHour = site["digest_hour"].as<int>();
  return row;
}

std::string seedMode(const GuardEnvironment& seed)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT value FROM guard_state WHERE key = 'mode'");
  if (rows.empty())
    return guardModeToString(seed.mode);
  return guardModeToString(guardModeFromString(rows.front()["value"].as<std::string>()));
}

bool insertDefaultEnvironment(const GuardEnvironment& seed)
{
  const SeedRow row = seedRow(seed);
  const std::string mode = seedMode(seed);
  const auto inserted = DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, is_default, mode, "
      "mode_updated_at, schedule_enabled, asleep_hours, open_hours, "
      "staffed_hours, closed_mode, digest_hour, quiet_policy, "
      "quiet_start_hour, quiet_end_hour) VALUES (?, ?, 1, ?, 0, ?, ?, ?, ?, "
      "?, ?, 'inherit', ?, ?) RETURNING id",
      seed.name, row.kind, mode, row.scheduleEnabled, row.asleep, row.open,
      row.staffed, row.closedMode, row.digestHour,
      std::clamp(seed.quietStartHour, 0, 23),
      std::clamp(seed.quietEndHour, 0, 23));
  if (inserted.empty())
    return false;
  const int64_t id = inserted.front()["id"].as<int64_t>();
  constexpr std::array<std::string_view, 3> kBackfills{
      "UPDATE guard_camera_context SET environment_id = ? WHERE "
      "environment_id = 0",
      "UPDATE guard_encounter SET environment_id = ? WHERE environment_id = 0",
      "UPDATE guard_decision_journal SET environment_id = ? WHERE "
      "environment_id = 0"};
  const auto client = DbService::client();
  for (const std::string_view statement : kBackfills)
    client->execSqlSync(std::string(statement), id);
  client->execSqlSync(
      "INSERT OR IGNORE INTO guard_state (key, value, updated_at) SELECT "
      "key || '_' || ?, value, updated_at FROM guard_state WHERE key IN "
      "('digest_quiet_day', 'digest_daily_day', 'digest_daily_until')",
      std::to_string(id));
  client->execSqlSync(
      "DELETE FROM guard_state WHERE key IN ('digest_quiet_day', "
      "'digest_daily_day', 'digest_daily_until', 'mode')");
  return exec("DROP TABLE IF EXISTS guard_site");
}

bool migrateGuestColumns()
{
  if (!tableExists("guard_expected_guest"))
    return true;
  if (!columnExists("guard_expected_guest", "camera_id") &&
      !exec("ALTER TABLE guard_expected_guest ADD COLUMN camera_id INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_expected_guest", "person_id") &&
      !exec("ALTER TABLE guard_expected_guest ADD COLUMN person_id INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_expected_guest", "host_user_id") &&
      !exec("ALTER TABLE guard_expected_guest ADD COLUMN host_user_id INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_expected_guest", "one_time") &&
      !exec("ALTER TABLE guard_expected_guest ADD COLUMN one_time INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!columnExists("guard_expected_guest", "used_at") &&
      !exec("ALTER TABLE guard_expected_guest ADD COLUMN used_at INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  return true;
}

}

bool guard_schema::migrate(const std::string& schemaPath)
{
  if (tableExists("guard_action") &&
      !columnExists("guard_action", "encounter_id") &&
      !exec("ALTER TABLE guard_action ADD COLUMN encounter_id INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (tableExists("guard_encounter") &&
      !columnExists("guard_encounter", "revision") &&
      !exec("ALTER TABLE guard_encounter ADD COLUMN revision INTEGER "
            "NOT NULL DEFAULT 0"))
    return false;
  if (!migrateGuestColumns() || !migrateEnvironmentColumns() ||
      !migrateInboxColumns() ||
      !migrateIncidentColumns() || !migrateActionColumns() ||
      !migrateEncounterDialogueColumns() || !migrateAssessmentColumns() ||
      !migrateEncounterNotifyColumns() || !migrateEncounterEpisodeColumns() ||
      !migrateJournalDispatchColumn() || !migrateJournalRound11Columns() ||
      !migrateJournalReasonsColumn() || !migrateOutboxColumns() ||
      !ensureDeadLetterTable() || !ensureBaselineTables() ||
      !ensureEncounterOutboxTable() || !ensureEvidenceIndex())
    return false;
  if (!encounterTableIsCurrent() && !rebuildEncounterTable(schemaPath))
    return false;
  if (!actionTableIsCurrent() && !rebuildActionTable(schemaPath))
    return false;
  if (!journalTableIsCurrent() && !rebuildJournalTable(schemaPath))
    return false;
  if (!inboxCheckIsCurrent() && !rebuildInboxTable(schemaPath))
    return false;
  if (!outboxStatusIsCurrent() && !rebuildActionOutboxTable(schemaPath))
    return false;
  return true;
}

bool guard_schema::seedEnvironments(const GuardEnvironment& seed)
{
  try {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT COUNT(*) AS total FROM guard_environment");
    if (!rows.empty() && rows.front()["total"].as<int>() > 0)
      return true;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard environment seed could not read the table: "
             << error.what();
    return false;
  }
  if (!exec("BEGIN IMMEDIATE"))
    return false;
  bool ok = false;
  try {
    ok = insertDefaultEnvironment(seed);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard environment seed failed: " << error.what();
    ok = false;
  }
  if (!ok) {
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}
