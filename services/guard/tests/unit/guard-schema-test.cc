#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/enums.hxx>
#include <shared/utils/schema-runner/schema-runner.hxx>
#include <sqlite3.h>

#include <array>
#include <string>

namespace
{

std::string scalar(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    return {};
  std::string value;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(statement, 0);
    if (text)
      value.assign(reinterpret_cast<const char*>(text));
  }
  sqlite3_finalize(statement);
  return value;
}

int execute(sqlite3* db, const std::string& sql)
{
  return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
}

struct TempDb
{
  sqlite3* db{nullptr};

  TempDb()
  {
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    REQUIRE(runSchemaFile(db, ARGUS_GUARD_SCHEMA_PATH));
  }

  ~TempDb()
  {
    if (db)
      sqlite3_close(db);
  }
};

} // namespace

TEST_CASE("every encounter state is accepted by the guard schema")
{
  TempDb fixture;
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_encounter (person_id, signature, "
                  "state, grade, checks, best_camera_id, best_score, "
                  "first_seen, last_seen) VALUES (1, '', 'observing', 'none', "
                  "0, 1, 0, 10, 10)") == SQLITE_OK);
  CHECK(scalar(fixture.db, "SELECT state FROM guard_encounter WHERE id = 1") ==
        "observing");

  constexpr std::array<EncounterState, 9> kStates = {
      EncounterState::Observing,   EncounterState::Verifying,
      EncounterState::Challenging, EncounterState::Listening,
      EncounterState::Interpreting, EncounterState::Resolved,
      EncounterState::Escalating,  EncounterState::Degraded,
      EncounterState::Closed};
  for (const EncounterState state : kStates) {
    const std::string value = encounterStateToString(state);
    CHECK(execute(fixture.db, "UPDATE guard_encounter SET state = '" + value +
                                  "' WHERE id = 1") == SQLITE_OK);
    CHECK(scalar(fixture.db, "SELECT state FROM guard_encounter WHERE id = 1") ==
          value);
  }

  CHECK(execute(fixture.db, "UPDATE guard_encounter SET state = 'observed' "
                            "WHERE id = 1") != SQLITE_OK);
}

TEST_CASE("guard actions persist their encounter link and effect kind")
{
  TempDb fixture;
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_action (incident_id, encounter_id, "
                  "camera_id, person_id, kind, status, detail, created_at) "
                  "VALUES (3, 4, 2, 1, 'announce', 'sent', '', 10)") ==
          SQLITE_OK);
  CHECK(scalar(fixture.db, "SELECT kind FROM guard_action WHERE id = 1") ==
        "announce");
  CHECK(scalar(fixture.db, "SELECT encounter_id FROM guard_action WHERE id = 1") ==
        "4");
}

TEST_CASE("guard action kinds round-trip through the shared enum")
{
  constexpr std::array<GuardActionKind, 8> kKinds = {
      GuardActionKind::Greet,       GuardActionKind::Listen,
      GuardActionKind::Reply,       GuardActionKind::Announce,
      GuardActionKind::Alarm,       GuardActionKind::SirenArm,
      GuardActionKind::SirenDisarm, GuardActionKind::Notify};
  for (const GuardActionKind kind : kKinds)
    CHECK(guardActionKindFromString(guardActionKindToString(kind)) == kind);
}

TEST_CASE("encounter states round-trip through the shared enum")
{
  constexpr std::array<EncounterState, 9> kStates = {
      EncounterState::Observing,   EncounterState::Verifying,
      EncounterState::Challenging, EncounterState::Listening,
      EncounterState::Interpreting, EncounterState::Resolved,
      EncounterState::Escalating,  EncounterState::Degraded,
      EncounterState::Closed};
  for (const EncounterState state : kStates)
    CHECK(encounterStateFromString(encounterStateToString(state)) == state);
}

TEST_CASE("the observation inbox deduplicates by event id")
{
  TempDb fixture;
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_observation_inbox (event_id, camera_id, "
                  "observation_id, received_at) VALUES ('e1', 1, 'o1', 10)") ==
          SQLITE_OK);
  REQUIRE(execute(fixture.db,
                  "INSERT OR IGNORE INTO guard_observation_inbox (event_id, "
                  "camera_id, observation_id, received_at) "
                  "VALUES ('e1', 1, 'o2', 11)") == SQLITE_OK);
  CHECK(scalar(fixture.db,
               "SELECT COUNT(*) FROM guard_observation_inbox WHERE "
               "event_id = 'e1'") == "1");
}

TEST_CASE("transitions and outbox rows persist the recovery trail")
{
  TempDb fixture;
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_encounter (person_id, signature, state, "
                  "grade, checks, revision, best_camera_id, best_score, "
                  "first_seen, last_seen) VALUES (1, '', 'observing', 'none', "
                  "0, 0, 1, 0, 10, 10)") == SQLITE_OK);
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_encounter_transition (encounter_id, "
                  "from_state, to_state, reason, revision, occurred_at) "
                  "VALUES (1, 'observing', 'escalating', 'escalated', 1, 10)") ==
          SQLITE_OK);
  CHECK(scalar(fixture.db,
               "SELECT to_state FROM guard_encounter_transition WHERE id = 1") ==
        "escalating");
  REQUIRE(execute(fixture.db,
                  "INSERT INTO guard_action_outbox (command_id, encounter_id, "
                  "incident_id, camera_id, person_id, kind, status, detail, "
                  "attempts, created_at, updated_at) VALUES ('c1', 1, 2, 1, 3, "
                  "'notify', 'pending', '', 0, 10, 10)") == SQLITE_OK);
  CHECK(scalar(fixture.db,
               "SELECT status FROM guard_action_outbox WHERE command_id = 'c1'") ==
        "pending");
}
