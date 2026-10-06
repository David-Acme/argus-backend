#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"
#include <doctest/doctest.h>

#include <feature/module-data/services/guard-module-data.hxx>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using guard_test::GuardBoot;
using guard_test::scalar;

namespace
{
std::int64_t itemCount(const ModuleDataSummary& summary, const std::string& kind)
{
  const auto found = std::ranges::find(summary.items, kind, &ModuleDataItem::kind);
  return found == summary.items.end() ? -1 : found->count;
}

std::string defaultEnvironment()
{
  return scalar("SELECT id FROM guard_environment WHERE is_default = 1");
}

void seed()
{
  const std::string home = defaultEnvironment();
  scalar("INSERT INTO guard_environment (id, name, kind) VALUES (50, 'Tienda', 'commercial')");
  scalar("INSERT INTO guard_incident (camera_id, camera_name, identity, event_json) VALUES (1, 'Patio', 'x', '{}')");
  scalar("INSERT INTO guard_action (incident_id, kind) VALUES (1, 'notify')");
  scalar("INSERT INTO guard_assessment (incident_id, caption) VALUES (1, 'una persona en la puerta')");
  scalar("INSERT INTO guard_encounter (id, first_seen, last_seen, environment_id) VALUES (7, 1, 2, 50)");
  scalar("INSERT INTO guard_encounter_transition (encounter_id, to_state, occurred_at) VALUES (7, 'closed', 2)");
  scalar("INSERT INTO guard_observation_inbox (event_id, payload) VALUES ('ev1', '{}')");
  scalar("INSERT INTO guard_dead_letter (event_id, payload) VALUES ('ev2', '{}')");
  scalar("INSERT INTO guard_action_outbox (command_id, payload) VALUES ('cmd1', '{}')");
  scalar("INSERT INTO guard_encounter_outbox (event_id, payload) VALUES ('enc1', '{}')");
  scalar("INSERT INTO guard_evidence (encounter_id, object_key, expires_at, deleted_at) VALUES "
         "(7, 'guard/incidents/7/a.jpg', 9999999999, 0), (7, 'guard/incidents/7/b.jpg', 1, 3)");
  scalar("INSERT INTO guard_hourly_baseline (camera_id, dow_hour) VALUES (1, 5)");
  scalar("INSERT INTO guard_signature_visit (signature) VALUES ('sig')");
  scalar("INSERT INTO guard_decision_journal (event_id, environment_id) VALUES ('ev1', 50)");
  scalar("INSERT INTO guard_expected_guest (description, valid_from, valid_until) VALUES ('Gasfitero', 1, 2)");
  scalar("INSERT INTO guard_camera_context (camera_id, environment_id) VALUES (1, 50)");
  scalar("INSERT INTO guard_state (key, value) VALUES ('tamper_onset_1', '1'), "
         "('response_directory_snapshot', '[]')");
  scalar("INSERT INTO guard_presence (user_id, environment_id, state, source, since, last_signal_at) VALUES "
         "(1, " + home + ", 'home', 'app_activity', 1, 1), (1, 50, 'away', 'timeout', 1, 1)");
  scalar("INSERT INTO guard_response_contact (environment_id, name, phone, updated_at) VALUES "
         "(" + home + ", 'Vecino', '999111222', 1), (50, 'Seguridad', '999333444', 1)");
  scalar("INSERT INTO guard_user_pin (user_id, disarm_hash, duress_hash, created_at, updated_at) VALUES "
         "(1, 'h1', 'h2', 1, 1)");
  scalar("INSERT INTO guard_safety_alert (kind, user_id, created_at) VALUES ('panic', 1, 1)");
}

struct Objects
{
  std::mutex mutex;
  std::vector<std::string> removed;
  bool ready{false};
  bool failing{false};
};
}

TEST_CASE("argus-guard purges what surveillance observed and keeps the household's safety records")
{
  GuardBoot boot("guard-module-data-test");
  seed();
  Objects objects;
  GuardModuleData data({.storageReady = [&objects] { return objects.ready; },
                        .removeObject = [&objects](std::string key) -> drogon::Task<void> {
                          const std::scoped_lock lock(objects.mutex);
                          if (objects.failing)
                            throw std::runtime_error("object storage refused");
                          objects.removed.push_back(std::move(key));
                          co_return;
                        },
                        .objectBudget = std::chrono::milliseconds(3000)});

  const auto before = data.summary("surveillance");
  CHECK(itemCount(before, "environments") == 1);
  CHECK(itemCount(before, "episodes") == 1);
  CHECK(itemCount(before, "incidents") == 1);
  CHECK(itemCount(before, "decisions") == 1);
  CHECK(itemCount(before, "expected_guests") == 1);
  CHECK(itemCount(before, "evidence_photos") == 1);
  CHECK(before.bytes > 0);
  CHECK(data.summary("productivity").empty());
  CHECK(data.purge("productivity").purged);

  scalar("ALTER TABLE guard_camera_context RENAME TO guard_camera_context_away");
  CHECK_THROWS(data.purge("surveillance"));
  CHECK(scalar("SELECT COUNT(*) FROM guard_incident") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_environment") == "2");
  scalar("ALTER TABLE guard_camera_context_away RENAME TO guard_camera_context");

  const auto withoutStorage = data.purge("surveillance");
  CHECK_FALSE(withoutStorage.purged);
  CHECK(withoutStorage.reason == "storage_unavailable");
  for (const auto* table :
       {"guard_incident", "guard_action", "guard_assessment", "guard_encounter", "guard_encounter_transition",
        "guard_observation_inbox", "guard_dead_letter", "guard_action_outbox", "guard_encounter_outbox",
        "guard_hourly_baseline", "guard_signature_visit", "guard_decision_journal", "guard_expected_guest",
        "guard_camera_context"})
    CHECK(scalar(std::string("SELECT COUNT(*) FROM ") + table) == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_environment") == "1");
  CHECK(scalar("SELECT is_default FROM guard_environment") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_presence") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_response_contact") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key = 'tamper_onset_1'") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key = 'response_directory_snapshot'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_user_pin") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_evidence") == "1");

  objects.ready = true;
  objects.failing = true;
  const auto refused = data.purge("surveillance");
  CHECK_FALSE(refused.purged);
  CHECK(refused.reason == "storage_failed");

  objects.failing = false;
  CHECK(data.purge("surveillance").purged);
  CHECK(scalar("SELECT COUNT(*) FROM guard_evidence") == "0");
  {
    const std::scoped_lock lock(objects.mutex);
    CHECK(objects.removed == std::vector<std::string>{"guard/incidents/7/a.jpg"});
  }
  CHECK(data.summary("surveillance").empty());
  CHECK(data.purge("surveillance").purged);
}
