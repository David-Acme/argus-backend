#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <json/value.h>
#include <notification/notification-client.hxx>
#include <unistd.h>
#include <optional>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <thread>

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

Json::Value scheduledObservation(const std::string& eventId, int64_t cameraId = 1)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = eventId;
  event["cameraId"] = Json::Int64(cameraId);
  event["rule"] = "person_day";
  event["severity"] = "info";
  event["trackId"] = Json::Int64(1);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(1);
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 32.0;
  bbox["h"] = 32.0;
  object["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

class CountingNotifications final : public NotificationClient
{
public:
  CountingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    calls.fetch_add(1);
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

class FakeCameraActions final : public CameraActionClient
{
public:
  FakeCameraActions()
      : CameraActionClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  CameraCommandResult announce(const CameraAnnounceInput&) const override
  {
    ++announceCalls;
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::SUCCEEDED;
    result.detail = "sent";
    return result;
  }

  CameraCommandResult listen(const CameraListenInput&) const override
  {
    ++listenCalls;
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::INDETERMINATE;
    result.detail = "capture_failed";
    return result;
  }

  CameraCommandResult alarm(const CameraAlarmInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::SUCCEEDED;
    return result;
  }

  CameraCommandResult setSiren(const CameraSirenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::REJECTED;
    return result;
  }

  std::optional<CameraCrop> personCrop(
      const CameraPersonCropInput&) const override
  {
    return std::nullopt;
  }

  mutable std::atomic<int> announceCalls{0};
  mutable std::atomic<int> listenCalls{0};
};

class FakeIdentity final : public IdentityClient
{
public:
  FakeIdentity() : IdentityClient("127.0.0.1:1", "fleet") {}

  std::optional<std::vector<int64_t>> listNotifiableUsers() const override
  {
    return std::vector<int64_t>{7};
  }
};
} // namespace

TEST_CASE("the observation saga is idempotent across redeliveries")
{
  const TempDb db("guard-saga-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  GuardRepository repository;

  const GuardInboxInput inbox = {.eventId = "evt:1",
                                 .cameraId = 1,
                                 .observationId = "1:2:3",
                                 .receivedAt = 100,
                                 .payload = {},
                                 .delivered = 0};
  const ObservationClaim first =
      drogon::sync_wait(repository.claimObservation(inbox));
  CHECK(first.kind == ObservationClaimKind::New);
  CHECK(first.attempts == 1);
  CHECK(first.stage == 0);

  CHECK(drogon::sync_wait(repository.advanceObservation(
      {.eventId = "evt:1",
       .stage = 3,
       .incidentId = 71,
       .encounterId = 91,
       .danger = "high",
       .checkpoint = "{\"dialogue\":{\"turns\":1}}",
       .at = 101})));

  const int64_t incidentId = drogon::sync_wait(
      repository.insertIncidentForEvent({.cameraId = 1,
                                         .cameraName = "door",
                                         .rule = "person_in_alert_zone",
                                         .danger = "high",
                                         .severity = "critical",
                                         .personId = 0,
                                         .identity = "unknown",
                                         .eventId = "evt:1",
                                         .eventJson = "{}",
                                         .createdAt = 101}));
  CHECK(incidentId > 0);
  CHECK(drogon::sync_wait(repository.insertIncidentForEvent(
            {.cameraId = 1,
             .cameraName = "door",
             .rule = "person_in_alert_zone",
             .danger = "high",
             .severity = "critical",
             .personId = 0,
             .identity = "unknown",
             .eventId = "evt:1",
             .eventJson = "{}",
             .createdAt = 101})) == incidentId);
  CHECK(
      scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = 'evt:1'") ==
      "1");

  const GuardOutboxInput intent = {.commandId = "evt:1:notify:1",
                                   .encounterId = 91,
                                   .incidentId = 71,
                                   .cameraId = 1,
                                   .personId = 0,
                                   .kind = "notify",
                                   .payload = {},
                                   .at = 101};
  const auto planned = drogon::sync_wait(repository.planIntent(intent));
  REQUIRE(planned.has_value());
  CHECK(planned->status == GuardIntentStatus::Pending);
  CHECK(drogon::sync_wait(
      repository.setOutboxPayload({.commandId = "evt:1:notify:1",
                                   .payload = "{\"userIds\":[7]}",
                                   .at = 101})));
  const auto replannedPayload =
      drogon::sync_wait(repository.planIntent(intent));
  REQUIRE(replannedPayload.has_value());
  CHECK(replannedPayload->payload == "{\"userIds\":[7]}");
  CHECK(drogon::sync_wait(repository.updateOutbox(
      {.commandId = "evt:1:notify:1",
       .status = GuardIntentStatus::Succeeded,
       .detail = "notified",
       .response = "{\"accepted\":true,\"executed\":true}",
       .nextAttemptAt = 0,
       .at = 102})));
  const auto replanned = drogon::sync_wait(repository.planIntent(intent));
  REQUIRE(replanned.has_value());
  CHECK(replanned->status == GuardIntentStatus::Succeeded);
  CHECK(replanned->detail == "notified");
  CHECK(replanned->response.find("accepted") != std::string::npos);

  const GuardActionInput action = {.incidentId = 71,
                                   .encounterId = 91,
                                   .cameraId = 1,
                                   .personId = 0,
                                   .commandId = "evt:1:notify:1",
                                   .kind = "notify",
                                   .status = "sent",
                                   .detail = "notified",
                                   .createdAt = 102};
  drogon::sync_wait(repository.insertAction(action));
  drogon::sync_wait(repository.insertAction(action));
  CHECK(scalar("SELECT COUNT(*) FROM guard_action "
               "WHERE command_id = 'evt:1:notify:1'") == "1");

  const ObservationClaim retry =
      drogon::sync_wait(repository.claimObservation(inbox));
  CHECK(retry.kind == ObservationClaimKind::Retry);
  CHECK(retry.attempts == 1);
  CHECK(retry.stage == 3);
  CHECK(retry.incidentId == 71);
  CHECK(retry.encounterId == 91);
  CHECK(retry.danger == "high");
  CHECK(retry.checkpoint.find("turns") != std::string::npos);

  CHECK(drogon::sync_wait(repository.completeObservation("evt:1", 200)));
  const ObservationClaim completed =
      drogon::sync_wait(repository.claimObservation(inbox));
  CHECK(completed.kind == ObservationClaimKind::Completed);

  CHECK(
      drogon::sync_wait(repository.insertDeadLetter({.eventId = "poison:1",
                                                     .payload = "{}",
                                                     .reason = "max_deliveries",
                                                     .attempts = 6,
                                                     .at = 300})));
  drogon::sync_wait(repository.insertDeadLetter({.eventId = "poison:1",
                                                 .payload = "{}",
                                                 .reason = "max_deliveries",
                                                 .attempts = 7,
                                                 .at = 301}));
  CHECK(scalar("SELECT COUNT(*) FROM guard_dead_letter") == "1");

  const GuardInboxInput poison = {.eventId = "poison:1",
                                  .cameraId = 1,
                                  .observationId = "1:1:1",
                                  .receivedAt = 399,
                                  .payload = {},
                                  .delivered = 0};
  const ObservationClaim claimedPoison =
      drogon::sync_wait(repository.claimObservation(poison));
  CHECK(claimedPoison.kind == ObservationClaimKind::New);
  CHECK(drogon::sync_wait(
      repository.markObservationDeadLettered("poison:1", 400)));
  CHECK(scalar("SELECT COUNT(*) FROM guard_observation_inbox WHERE "
               "event_id = 'poison:1' AND status = 'dead_lettered'") == "1");
  const ObservationClaim dead =
      drogon::sync_wait(repository.claimObservation(poison));
  CHECK(dead.kind == ObservationClaimKind::Completed);

  const GuardIncidentPhaseInput faultPhase =
      {.incident = {.cameraId = 1,
                    .cameraName = "door",
                    .rule = "person_in_alert_zone",
                    .danger = "high",
                    .severity = "critical",
                    .personId = 0,
                    .identity = "unknown",
                    .eventId = "fault:1",
                    .eventJson = "{}",
                    .createdAt = 200},
       .consumeGuestId = 0,
       .consumeGuestAt = 0,
       .advance = {.eventId = "fault:1",
                   .stage = 1,
                   .incidentId = 0,
                   .encounterId = 0,
                   .danger = "high",
                   .checkpoint = "{}",
                   .at = 200}};
  const ObservationClaim faultClaim =
      drogon::sync_wait(repository.claimObservation({.eventId = "fault:1",
                                                     .cameraId = 1,
                                                     .observationId = "fault:1",
                                                     .receivedAt = 199,
                                                     .payload = {},
                                                     .delivered = 0}));
  CHECK(faultClaim.kind == ObservationClaimKind::New);
  const GuardIncidentPhaseResult faultFirst =
      drogon::sync_wait(repository.commitIncidentPhase(faultPhase));
  CHECK(faultFirst.committed);
  CHECK(faultFirst.incidentId > 0);
  const GuardIncidentPhaseResult faultReplay =
      drogon::sync_wait(repository.commitIncidentPhase(faultPhase));
  CHECK(faultReplay.committed);
  CHECK(faultReplay.incidentId == faultFirst.incidentId);
  CHECK(scalar("SELECT COUNT(*) FROM guard_incident "
               "WHERE event_id = 'fault:1'") == "1");
  CHECK(scalar("SELECT stage FROM guard_observation_inbox "
               "WHERE event_id = 'fault:1'") == "1");

  const int64_t guestId =
      drogon::sync_wait(repository.insertGuest({.description = "visitor",
                                                .cameraId = 1,
                                                .personId = 0,
                                                .hostUserId = 0,
                                                .oneTime = true,
                                                .validFrom = 150,
                                                .validUntil = 300}));
  CHECK(guestId > 0);
  CHECK(
      drogon::sync_wait(repository.claimObservation({.eventId = "fault:2",
                                                     .cameraId = 1,
                                                     .observationId = "fault:2",
                                                     .receivedAt = 249,
                                                     .payload = {},
                                                     .delivered = 0}))
          .kind == ObservationClaimKind::New);
  GuardIncidentPhaseInput guestPhase = faultPhase;
  guestPhase.incident.eventId = "fault:2";
  guestPhase.advance.eventId = "fault:2";
  guestPhase.consumeGuestId = guestId;
  guestPhase.consumeGuestAt = 250;
  const GuardIncidentPhaseResult guestFirst =
      drogon::sync_wait(repository.commitIncidentPhase(guestPhase));
  CHECK(guestFirst.committed);
  const GuardIncidentPhaseResult guestReplay =
      drogon::sync_wait(repository.commitIncidentPhase(guestPhase));
  CHECK(guestReplay.incidentId == guestFirst.incidentId);
  CHECK(scalar("SELECT used_at FROM guard_expected_guest WHERE id = " +
               std::to_string(guestId)) == "250");

  CHECK(drogon::sync_wait(repository.claimObservation({.eventId = "enc:1",
                                                       .cameraId = 1,
                                                       .observationId = "enc:1",
                                                       .receivedAt = 500,
                                                       .payload = {},
                                                       .delivered = 0}))
            .kind == ObservationClaimKind::New);
  const GuardEncounterPhaseInput encounterPhase =
      {.create = true,
       .encounterIdToTouch = 0,
       .createInput = {.personId = 0,
                       .signature = {},
                       .bestCameraId = 1,
                       .bestScore = 1.0,
                       .at = 500},
       .touchInput = {},
       .closeForPerson = false,
       .closePersonId = 0,
       .closeAt = 0,
       .advance = {.eventId = "enc:1",
                   .stage = 2,
                   .incidentId = 0,
                   .encounterId = 0,
                   .danger = "medium",
                   .checkpoint = "{}",
                   .at = 500}};
  const GuardEncounterPhaseResult encounterResult =
      drogon::sync_wait(repository.commitEncounterPhase(encounterPhase));
  CHECK(encounterResult.committed);
  CHECK(encounterResult.encounterId > 0);
  CHECK(encounterResult.encounterChecks == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter") == "1");
  CHECK(scalar("SELECT stage FROM guard_observation_inbox "
               "WHERE event_id = 'enc:1'") == "2");
  CHECK(scalar("SELECT value FROM json_each("
               "(SELECT checkpoint FROM guard_observation_inbox "
               "WHERE event_id = 'enc:1')) WHERE key = 'encounterChecks'") ==
        "1");

  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounterResult.encounterId,
       .toState = EncounterState::Observing,
       .reason = "observed",
       .grade = "none",
       .at = 501})));
  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounterResult.encounterId,
       .toState = EncounterState::Observing,
       .reason = "observed",
       .grade = "none",
       .at = 502})));
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter_transition "
               "WHERE encounter_id = " +
               std::to_string(encounterResult.encounterId)) == "0");

  drogon::sync_wait(repository.insertAssessment({.incidentId = 71,
                                                 .cameraId = 1,
                                                 .eventId = "evt:1",
                                                 .mode = "agent",
                                                 .caption = "person",
                                                 .threat = "low",
                                                 .veto = false,
                                                 .tags = "[]",
                                                 .summary = "one",
                                                 .createdAt = 600}));
  drogon::sync_wait(repository.insertAssessment({.incidentId = 71,
                                                 .cameraId = 1,
                                                 .eventId = "evt:1",
                                                 .mode = "agent",
                                                 .caption = "person",
                                                 .threat = "low",
                                                 .veto = false,
                                                 .tags = "[]",
                                                 .summary = "one",
                                                 .createdAt = 601}));
  CHECK(scalar("SELECT COUNT(*) FROM guard_assessment "
               "WHERE event_id = 'evt:1'") == "1");
  drogon::sync_wait(
      repository.insertEvidence({.incidentId = 71,
                                 .encounterId = 91,
                                 .cameraId = 1,
                                 .objectKey = "guard/incidents/1/71.json",
                                 .contentType = "application/json",
                                 .retentionClass = "standard",
                                 .createdAt = 700,
                                 .expiresAt = 800}));
  drogon::sync_wait(
      repository.insertEvidence({.incidentId = 71,
                                 .encounterId = 91,
                                 .cameraId = 1,
                                 .objectKey = "guard/incidents/1/71.json",
                                 .contentType = "application/json",
                                 .retentionClass = "standard",
                                 .createdAt = 701,
                                 .expiresAt = 801}));
  CHECK(scalar("SELECT COUNT(*) FROM guard_evidence "
               "WHERE object_key = 'guard/incidents/1/71.json'") == "1");

  {
    const GuardOutboxInput intent = {.commandId = "state:1",
                                     .encounterId = 0,
                                     .incidentId = 0,
                                     .cameraId = 1,
                                     .personId = 0,
                                     .kind = "announce",
                                     .payload = {},
                                     .at = 2000};
    const auto pending = drogon::sync_wait(repository.planIntent(intent));
    REQUIRE(pending.has_value());
    CHECK(pending->status == GuardIntentStatus::Pending);
    CHECK(guardIntentStatusIsResumable(pending->status));
    CHECK(drogon::sync_wait(
        repository.updateOutbox({.commandId = "state:1",
                                 .status = GuardIntentStatus::RetryableFailed,
                                 .detail = "transient",
                                 .response = "{}",
                                 .nextAttemptAt = 2600,
                                 .at = 2001})));
    const auto retryable = drogon::sync_wait(repository.planIntent(intent));
    REQUIRE(retryable.has_value());
    CHECK(retryable->status == GuardIntentStatus::RetryableFailed);
    CHECK(retryable->nextAttemptAt == 2600);
    CHECK(guardIntentStatusIsResumable(retryable->status));
    CHECK(drogon::sync_wait(
        repository.updateOutbox({.commandId = "state:1",
                                 .status = GuardIntentStatus::Indeterminate,
                                 .detail = "crash",
                                 .response = "{}",
                                 .nextAttemptAt = 0,
                                 .at = 2002})));
    const auto indeterminate = drogon::sync_wait(repository.planIntent(intent));
    REQUIRE(indeterminate.has_value());
    CHECK(indeterminate->status == GuardIntentStatus::Indeterminate);
    CHECK(guardIntentStatusIsTerminal(indeterminate->status));
  }

  {
    const GuardEncounterPhaseInput createPhase =
        {.create = true,
         .encounterIdToTouch = 0,
         .createInput = {.personId = 42,
                         .signature = {},
                         .bestCameraId = 3,
                         .bestScore = 1.0,
                         .at = 700},
         .touchInput = {},
         .closeForPerson = false,
         .closePersonId = 0,
         .closeAt = 0,
         .advance = {.eventId = "enc:close",
                     .stage = 2,
                     .incidentId = 0,
                     .encounterId = 0,
                     .danger = "none",
                     .checkpoint = "{}",
                     .at = 700}};
    const auto created =
        drogon::sync_wait(repository.commitEncounterPhase(createPhase));
    REQUIRE(created.committed);
    const GuardEncounterPhaseInput closePhase =
        {.create = false,
         .encounterIdToTouch = created.encounterId,
         .createInput = {},
         .touchInput = {.id = created.encounterId,
                        .bestCameraId = 3,
                        .bestScore = 1.0,
                        .lastSeen = 800},
         .closeForPerson = true,
         .closePersonId = 42,
         .closeAt = 800,
         .advance = {.eventId = "enc:close",
                     .stage = 3,
                     .incidentId = 0,
                     .encounterId = created.encounterId,
                     .danger = "none",
                     .checkpoint = "{}",
                     .at = 800}};
    const auto closed =
        drogon::sync_wait(repository.commitEncounterPhase(closePhase));
    REQUIRE(closed.committed);
    REQUIRE(closed.closed.size() == 1);
    const std::string eventId =
        encounterClosedEventId(closed.closed.front(), 800);
    CHECK(scalar(
              "SELECT COUNT(*) FROM guard_encounter_outbox WHERE event_id = '" +
              eventId + "'") == "1");
    CHECK(
        scalar("SELECT status FROM guard_encounter_outbox WHERE event_id = '" +
               eventId + "'") == "pending");
    CHECK(
        scalar("SELECT payload FROM guard_encounter_outbox WHERE event_id = '" +
               eventId + "'")
            .find("\"personId\":42") != std::string::npos);
  }

  {
    const GuardInboxInput seed = {.eventId = "retry-payload:1",
                                  .cameraId = 1,
                                  .observationId = "retry-payload:1",
                                  .receivedAt = 2100,
                                  .payload = {},
                                  .delivered = 1};
    REQUIRE(drogon::sync_wait(repository.claimObservation(seed)).kind ==
            ObservationClaimKind::New);
    const std::string durable =
        "{\"eventId\":\"retry-payload:1\",\"cameraId\":1}";
    const ObservationClaim recovered = drogon::sync_wait(
        repository.claimObservation({.eventId = "retry-payload:1",
                                     .cameraId = 1,
                                     .observationId = "retry-payload:1",
                                     .receivedAt = 2101,
                                     .payload = durable,
                                     .delivered = 2}));
    CHECK(recovered.kind == ObservationClaimKind::Retry);
    CHECK(recovered.payload == durable);
    CHECK(recovered.attempts == 2);
    CHECK(scalar("SELECT payload FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-payload:1'") == durable);
  }

  {
    const GuardInboxInput terminal = {.eventId = "dead:1",
                                      .cameraId = 1,
                                      .observationId = "dead:1",
                                      .receivedAt = 2102,
                                      .payload = {},
                                      .delivered = 1};
    REQUIRE(drogon::sync_wait(repository.claimObservation(terminal)).kind ==
            ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(
        repository.markObservationDeadLettered("dead:1", 2103)));
    const ObservationClaim deadRetry =
        drogon::sync_wait(repository.claimObservation(terminal));
    CHECK(deadRetry.kind == ObservationClaimKind::Completed);
    CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE "
                 "event_id = 'dead:1'") == "dead_lettered");
  }

  {
    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const GuardInboxInput scheduledSeed = {.eventId = "retry-scheduled:1",
                                           .cameraId = 1,
                                           .observationId = "retry-scheduled:1",
                                           .receivedAt = 2200,
                                           .payload = {"retry-scheduled:1"},
                                           .delivered = 1};
    REQUIRE(drogon::sync_wait(repository.claimObservation(scheduledSeed)).kind ==
            ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(repository.setInboxRetry(
        {.eventId = "retry-scheduled:1",
         .payload = "{\"eventId\":\"retry-scheduled:1\"}",
         .retryAt = nowMs + 60000,
         .at = 2201})));
    const ObservationClaim scheduledRedelivery = drogon::sync_wait(
        repository.claimObservation({.eventId = "retry-scheduled:1",
                                     .cameraId = 1,
                                     .observationId = "retry-scheduled:1",
                                     .receivedAt = 2202,
                                     .payload = {},
                                     .delivered = 2}));
    CHECK(scheduledRedelivery.kind == ObservationClaimKind::Scheduled);
    CHECK(scheduledRedelivery.localRetries == 1);
    CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-scheduled:1'") == "processing");
    CHECK(scalar("SELECT retry_at FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-scheduled:1'") ==
          std::to_string(nowMs + 60000));

    const GuardInboxInput leasedSeed = {.eventId = "retry-lease:1",
                                        .cameraId = 1,
                                        .observationId = "retry-lease:1",
                                        .receivedAt = 2210,
                                        .payload = {"retry-lease:1"},
                                        .delivered = 1};
    REQUIRE(drogon::sync_wait(repository.claimObservation(leasedSeed)).kind ==
            ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(repository.setInboxRetry(
        {.eventId = "retry-lease:1",
         .payload = "{\"eventId\":\"retry-lease:1\"}",
         .retryAt = nowMs - 1000,
         .at = 2211})));
    const ObservationClaim leased = drogon::sync_wait(
        repository.claimObservation({.eventId = "retry-lease:1",
                                     .cameraId = 1,
                                     .observationId = "retry-lease:1",
                                     .receivedAt = 2212,
                                     .payload = {},
                                     .delivered = 2}));
    CHECK(leased.kind == ObservationClaimKind::Retry);
    CHECK(leased.localRetries == 1);
    CHECK(leased.retryAt > nowMs);
    const ObservationClaim leasedDuplicate = drogon::sync_wait(
        repository.claimObservation({.eventId = "retry-lease:1",
                                     .cameraId = 1,
                                     .observationId = "retry-lease:1",
                                     .receivedAt = 2213,
                                     .payload = {},
                                     .delivered = 3}));
    CHECK(leasedDuplicate.kind == ObservationClaimKind::Scheduled);
  }

  {
    GuardService::Config serviceConfig;
    serviceConfig.enabled = true;
    GuardService service({.bus = nullptr,
                          .identity = nullptr,
                          .notifications = nullptr,
                          .actions = nullptr,
                          .assessment = nullptr},
                         serviceConfig);
    const int64_t serviceNowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const Json::Value duplicateEvent = scheduledObservation("retry-service:1");
    const std::string duplicatePayload = json_util::toString(duplicateEvent);
    REQUIRE(drogon::sync_wait(repository.claimObservation(
                {.eventId = "retry-service:1",
                 .cameraId = 1,
                 .observationId = "retry-service:1",
                 .receivedAt = 2300,
                 .payload = duplicatePayload,
                 .delivered = 1}))
                .kind == ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(repository.setInboxRetry(
        {.eventId = "retry-service:1",
         .payload = duplicatePayload,
         .retryAt = serviceNowMs + 60000,
         .at = 2301})));
    CHECK(drogon::sync_wait(service.handle(duplicateEvent, 2)));
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE "
                 "event_id = 'retry-service:1'") == "0");
    CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-service:1'") == "processing");

    const Json::Value leasedEvent = scheduledObservation("retry-service:2");
    const std::string leasedPayload = json_util::toString(leasedEvent);
    REQUIRE(drogon::sync_wait(repository.claimObservation(
                {.eventId = "retry-service:2",
                 .cameraId = 1,
                 .observationId = "retry-service:2",
                 .receivedAt = 2310,
                 .payload = leasedPayload,
                 .delivered = 1}))
                .kind == ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(repository.setInboxRetry(
        {.eventId = "retry-service:2",
         .payload = leasedPayload,
         .retryAt = serviceNowMs - 1000,
         .at = 2311})));
    CHECK(drogon::sync_wait(service.handleLocalRetry(leasedEvent)));
    CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-service:2'") == "completed");
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE "
                 "event_id = 'retry-service:2'") == "1");
  }

  {
    const std::string commandId = "replay-service:1:notify:1";
    REQUIRE(drogon::sync_wait(repository.planIntent(
                {.commandId = commandId,
                 .encounterId = 0,
                 .incidentId = 0,
                 .cameraId = 1,
                 .personId = 0,
                 .kind = "notify",
                 .payload = {},
                 .at = 4000}))
                .has_value());
    REQUIRE(drogon::sync_wait(repository.updateOutbox(
        {.commandId = commandId,
         .status = GuardIntentStatus::Rejected,
         .detail = "denied",
         .response = "{\"authorized\":false,\"accepted\":false,\"pending\":"
                     "false,\"indeterminate\":false,\"executed\":false,"
                     "\"detail\":\"denied\",\"speechDetected\":false,"
                     "\"heard\":\"\"}",
         .nextAttemptAt = 0,
         .at = 4001})));
    CHECK_FALSE(drogon::sync_wait(repository.updateOutbox(
        {.commandId = "ghost:1",
         .status = GuardIntentStatus::Succeeded,
         .detail = "ghost",
         .response = "{}",
         .nextAttemptAt = 0,
         .at = 4002})));

    GuardService::Config replayConfig;
    replayConfig.enabled = true;
    replayConfig.stagingEnabled = false;
    replayConfig.notifyLevel = 1;
    CountingNotifications counting;
    GuardService replayService({.bus = nullptr,
                                .identity = nullptr,
                                .notifications = &counting,
                                .actions = nullptr,
                                .assessment = nullptr},
                               replayConfig);
    CHECK(drogon::sync_wait(
        replayService.handle(scheduledObservation("replay-service:1"), 1)));
    CHECK(counting.calls.load() == 0);
    CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = "
                 "'replay-service:1:notify:1'") == "rejected");
  }

  {
    const std::string commandId = "replay-listen:1:greet_listen:2";
    REQUIRE(drogon::sync_wait(repository.planIntent(
                {.commandId = commandId,
                 .encounterId = 0,
                 .incidentId = 0,
                 .cameraId = 2,
                 .personId = 0,
                 .kind = "greet_listen",
                 .payload = {},
                 .at = 5000}))
                .has_value());
    REQUIRE(drogon::sync_wait(repository.updateOutbox(
        {.commandId = commandId,
         .status = GuardIntentStatus::Rejected,
         .detail = "denied",
         .response = "{\"authorized\":false,\"accepted\":false,\"pending\":"
                     "false,\"indeterminate\":false,\"executed\":false,"
                     "\"detail\":\"denied\",\"speechDetected\":false,"
                     "\"heard\":\"\"}",
         .nextAttemptAt = 0,
         .at = 5001})));

    GuardService::Config listenConfig;
    listenConfig.enabled = true;
    listenConfig.greetEnabled = true;
    listenConfig.greetTexts = {"hello"};
    listenConfig.greetListenSeconds = 5;
    FakeCameraActions actions;
    FakeIdentity identity;
    CountingNotifications notifications;
    GuardService listenService({.bus = nullptr,
                                .identity = &identity,
                                .notifications = &notifications,
                                .actions = &actions,
                                .assessment = nullptr},
                               listenConfig);
    CHECK(drogon::sync_wait(
        listenService.handle(scheduledObservation("replay-listen:1", 2), 1)));
    CHECK(actions.listenCalls.load() == 0);
    CHECK(actions.announceCalls.load() == 1);
    CHECK(scalar("SELECT COUNT(*) FROM guard_action WHERE command_id = "
                 "'replay-listen:1:greet:1'") == "1");
    CHECK(scalar("SELECT COUNT(*) FROM guard_action WHERE command_id = "
                 "'replay-listen:1:greet_listen:2'") == "0");
    CHECK(scalar("SELECT json_extract(checkpoint, '$.dialogue.listened') "
                 "FROM guard_observation_inbox WHERE event_id = "
                 "'replay-listen:1'") == "0");
    CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = "
                 "'replay-listen:1:greet_listen:2'") == "rejected");
  }

  drogon::app().quit();
  runner.join();
}
