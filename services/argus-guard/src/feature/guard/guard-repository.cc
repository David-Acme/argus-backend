#include "guard-repository.hxx"

#include <chrono>
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <string>

using namespace guard_query;

namespace
{

// Commits in the transaction destructor; the callback resumes the awaiter.
class TransactionCommitAwaiter
{
public:
  explicit TransactionCommitAwaiter(
      std::shared_ptr<drogon::orm::Transaction> transaction)
      : transaction_(std::move(transaction))
  {
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) noexcept
  {
    auto transaction = std::move(transaction_);
    if (!transaction) {
      committed_ = false;
      handle.resume();
      return;
    }
    transaction->setCommitCallback([this, handle](bool committed) {
      committed_ = committed;
      handle.resume();
    });
    transaction.reset();
  }

  bool await_resume() const noexcept { return committed_; }

private:
  std::shared_ptr<drogon::orm::Transaction> transaction_;
  bool committed_{false};
};
GuardEncounter encounterFromRow(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .personId = row["person_id"].as<int64_t>(),
          .signature = row["signature"].as<std::string>(),
          .state = encounterStateFromString(row["state"].as<std::string>()),
          .grade = row["grade"].as<std::string>(),
          .checks = row["checks"].as<int>(),
          .bestCameraId = row["best_camera_id"].as<int64_t>(),
          .bestScore = row["best_score"].as<double>(),
          .dialogueTurns = row["dialogue_turns"].as<int>(),
          .dialogueGoal = row["dialogue_goal"].as<std::string>(),
          .listeningUntil = row["listening_until"].as<int64_t>(),
          .lastLine = row["last_line"].as<std::string>(),
          .lastHeard = row["last_heard"].as<std::string>(),
          .lastHeardAt = row["last_heard_at"].as<int64_t>(),
          .firstSeen = row["first_seen"].as<int64_t>(),
          .lastSeen = row["last_seen"].as<int64_t>()};
}

int64_t inboxNowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kInboxRetryLeaseMs = 60000;
} // namespace

drogon::Task<int64_t>
GuardRepository::insertIncident(const GuardIncidentInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_INCIDENT.data(), input.cameraId,
                                   input.cameraName, input.rule, input.danger,
                                   input.severity, input.personId,
                                   input.identity, input.eventId,
                                   input.eventJson, input.createdAt);
  co_return result.insertId();
}

drogon::Task<int64_t>
GuardRepository::insertIncidentForEvent(const GuardIncidentInput& input) const
{
  auto client = DbService::client();
  if (input.eventId.empty())
    co_return co_await insertIncident(input);
  const auto result =
      co_await client->execSqlCoro(INSERT_INCIDENT_EVENT.data(), input.cameraId,
                                   input.cameraName, input.rule, input.danger,
                                   input.severity, input.personId,
                                   input.identity, input.eventId,
                                   input.eventJson, input.createdAt);
  if (result.insertId() > 0)
    co_return result.insertId();
  const auto existing =
      co_await client->execSqlCoro(SELECT_INCIDENT_BY_EVENT.data(),
                                   input.eventId);
  co_return existing.empty() ? 0 : existing.front()["id"].as<int64_t>();
}

drogon::Task<bool>
GuardRepository::updateIncidentDanger(int64_t id,
                                      const std::string& danger) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPDATE_INCIDENT_DANGER.data(), danger, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
GuardRepository::insertAction(const GuardActionInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_ACTION.data(), input.incidentId,
                                   input.encounterId, input.cameraId,
                                   input.personId, input.commandId, input.kind,
                                   input.status, input.detail, input.createdAt);
  co_return result.insertId();
}

drogon::Task<int> GuardRepository::repeatCount(int64_t personId,
                                               int64_t since) const
{
  if (personId <= 0)
    co_return 0;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(COUNT_PERSON_SINCE.data(), personId, since);
  if (result.empty())
    co_return 0;
  co_return result.front()["total"].as<int>();
}

drogon::Task<bool> GuardRepository::canAct(const GuardCanActInput& input) const
{
  auto client = DbService::client();
  const auto last =
      co_await client->execSqlCoro(LAST_ACTION.data(), input.cameraId,
                                   input.personId);
  if (!last.empty()) {
    const int64_t at = last.front()["created_at"].as<int64_t>();
    if (input.now - at < input.cooldownS)
      co_return false;
  }

  if (input.maxPerHour > 0) {
    const auto count =
        co_await client->execSqlCoro(COUNT_ACTIONS_SINCE.data(), input.cameraId,
                                     input.now - 3600);
    if (!count.empty() &&
        count.front()["total"].as<int64_t>() >= input.maxPerHour)
      co_return false;
  }
  co_return true;
}

drogon::Task<int64_t> GuardRepository::effectsSince(int64_t cameraId,
                                                    int64_t since) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(COUNT_ACTIONS_SINCE.data(), cameraId, since);
  if (result.empty())
    co_return 0;
  co_return result.front()["total"].as<int64_t>();
}

drogon::Task<std::string>
GuardRepository::state(const std::string& key,
                       const std::string& fallback) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(SEL_STATE.data(), key);
  if (result.empty())
    co_return fallback;
  co_return result.front()["value"].as<std::string>();
}

drogon::Task<bool> GuardRepository::setState(const GuardStateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPSERT_STATE.data(), input.key, input.value,
                                   input.updatedAt);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<Json::Value>>
GuardRepository::recentIncidents(int limit) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(SELECT_RECENT_INCIDENTS.data(),
                                   limit > 0 ? limit : 20);
  std::vector<Json::Value> incidents;
  incidents.reserve(result.size());
  for (const auto& row : result) {
    Json::Value incident;
    incident["cameraId"] = Json::Int64(row["camera_id"].as<int64_t>());
    incident["cameraName"] = row["camera_name"].as<std::string>();
    incident["rule"] = row["rule"].as<std::string>();
    incident["danger"] = row["danger"].as<std::string>();
    incident["severity"] = row["severity"].as<std::string>();
    incident["personId"] = Json::Int64(row["person_id"].as<int64_t>());
    incident["identity"] = row["identity"].as<std::string>();
    incident["createdAt"] = Json::Int64(row["created_at"].as<int64_t>());
    incidents.push_back(std::move(incident));
  }
  co_return incidents;
}

drogon::Task<int64_t>
GuardRepository::insertGuest(const GuardGuestInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_GUEST.data(), input.description,
                                   input.cameraId, input.personId,
                                   input.hostUserId, input.oneTime ? 1 : 0,
                                   input.validFrom, input.validUntil,
                                   static_cast<int64_t>(std::time(nullptr)));
  co_return result.insertId();
}

drogon::Task<std::optional<GuardGuest>>
GuardRepository::activeGuest(int64_t at, int64_t cameraId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(ACTIVE_GUEST.data(), at, at, cameraId);
  if (result.empty())
    co_return std::nullopt;
  const auto& row = result.front();
  co_return GuardGuest{.id = row["id"].as<int64_t>(),
                       .description = row["description"].as<std::string>(),
                       .cameraId = row["camera_id"].as<int64_t>(),
                       .personId = row["person_id"].as<int64_t>(),
                       .hostUserId = row["host_user_id"].as<int64_t>(),
                       .oneTime = row["one_time"].as<int>() != 0,
                       .validFrom = 0,
                       .validUntil = 0};
}

drogon::Task<bool> GuardRepository::consumeGuest(int64_t id, int64_t at) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(CONSUME_GUEST.data(), at, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<GuardGuest>> GuardRepository::listGuests() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(LIST_GUESTS.data());
  std::vector<GuardGuest> guests;
  guests.reserve(result.size());
  for (const auto& row : result) {
    guests.push_back({.id = row["id"].as<int64_t>(),
                      .description = row["description"].as<std::string>(),
                      .cameraId = row["camera_id"].as<int64_t>(),
                      .personId = row["person_id"].as<int64_t>(),
                      .hostUserId = row["host_user_id"].as<int64_t>(),
                      .oneTime = row["one_time"].as<int>() != 0,
                      .validFrom = row["valid_from"].as<int64_t>(),
                      .validUntil = row["valid_until"].as<int64_t>()});
  }
  co_return guests;
}

drogon::Task<bool> GuardRepository::removeGuest(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(DELETE_GUEST.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
GuardRepository::insertAssessment(const GuardAssessmentRowInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_ASSESSMENT.data(), input.incidentId,
                                   input.cameraId, input.eventId, input.mode,
                                   input.caption, input.threat,
                                   input.veto ? 1 : 0, input.tags,
                                   input.summary, input.createdAt);
  co_return result.insertId();
}

drogon::Task<std::vector<GuardEncounter>>
GuardRepository::openEncounters(int64_t since) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(OPEN_ENCOUNTERS.data(), since);
  std::vector<GuardEncounter> encounters;
  encounters.reserve(result.size());
  for (const auto& row : result)
    encounters.push_back(encounterFromRow(row));
  co_return encounters;
}

drogon::Task<std::vector<GuardEncounter>>
GuardRepository::staleEncounters(int64_t olderThan) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(STALE_ENCOUNTERS.data(), olderThan);
  std::vector<GuardEncounter> encounters;
  encounters.reserve(result.size());
  for (const auto& row : result)
    encounters.push_back(encounterFromRow(row));
  co_return encounters;
}

drogon::Task<std::vector<GuardEncounter>>
GuardRepository::encountersForPerson(int64_t personId) const
{
  if (personId <= 0)
    co_return {};
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(ENCOUNTERS_FOR_PERSON.data(), personId);
  std::vector<GuardEncounter> encounters;
  encounters.reserve(result.size());
  for (const auto& row : result)
    encounters.push_back(encounterFromRow(row));
  co_return encounters;
}

drogon::Task<std::optional<GuardEncounter>>
GuardRepository::findEncounter(int64_t encounterId) const
{
  if (encounterId <= 0)
    co_return std::nullopt;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_ENCOUNTER.data(), encounterId);
  if (result.empty())
    co_return std::nullopt;
  co_return encounterFromRow(result.front());
}

drogon::Task<bool>
GuardRepository::recordDialogue(const GuardDialogueInput& input) const
{
  if (input.encounterId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(RECORD_DIALOGUE.data(), input.turnKey,
                                   input.turnKey, input.goal,
                                   input.listeningUntil, input.lastLine,
                                   input.lastHeard, input.at,
                                   input.encounterId);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
GuardRepository::createEncounter(const GuardEncounterCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_ENCOUNTER.data(), input.personId,
                                   input.signature, input.bestCameraId,
                                   input.bestScore, input.at, input.at);
  co_return result.insertId();
}

drogon::Task<int>
GuardRepository::touchEncounter(const GuardEncounterTouchInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(TOUCH_ENCOUNTER.data(), input.bestCameraId,
                                   input.bestScore, input.lastSeen, input.id);
  if (result.empty())
    co_return 0;
  co_return result.front()["checks"].as<int>();
}

drogon::Task<bool>
GuardRepository::transitionEncounter(const GuardTransitionInput& input) const
{
  if (input.encounterId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto current =
      co_await client->execSqlCoro(SELECT_ENCOUNTER_STATE.data(),
                                   input.encounterId);
  if (current.empty())
    co_return false;
  const std::string from = current.front()["state"].as<std::string>();
  if (from == encounterStateToString(input.toState))
    co_return true;
  const int64_t revision = current.front()["revision"].as<int64_t>() + 1;
  const auto updated =
      co_await client->execSqlCoro(UPDATE_ENCOUNTER_STATE_REVISION.data(),
                                   input.grade,
                                   encounterStateToString(input.toState),
                                   input.encounterId);
  if (updated.affectedRows() <= 0)
    co_return false;
  co_await client->execSqlCoro(INSERT_TRANSITION.data(), input.encounterId,
                               from, encounterStateToString(input.toState),
                               input.reason, revision, input.at);
  co_return true;
}

drogon::Task<ObservationClaim>
GuardRepository::claimObservation(const GuardInboxInput& input) const
{
  ObservationClaim claim;
  if (input.eventId.empty())
    co_return claim;
  auto client = DbService::client();
  const auto existing =
      co_await client->execSqlCoro(SELECT_INBOX.data(), input.eventId);
  if (!existing.empty()) {
    const auto& row = existing.front();
    if (observationStatusFromString(row["status"].as<std::string>()) ==
        ObservationStatus::Processing) {
      const int storedAttempts = row["attempts"].as<int>();
      const int64_t storedRetryAt = row["retry_at"].as<int64_t>();
      if (storedRetryAt > 0) {
        const int64_t claimNowMs = inboxNowMillis();
        if (storedRetryAt <= claimNowMs) {
          const int64_t leaseUntil = claimNowMs + kInboxRetryLeaseMs;
          const auto leased = co_await client->execSqlCoro(
              CLAIM_INBOX_RETRY.data(), leaseUntil, input.eventId, claimNowMs);
          if (leased.affectedRows() > 0) {
            claim.kind = ObservationClaimKind::Retry;
            claim.attempts = std::max(storedAttempts, input.delivered);
            claim.localRetries = row["local_retries"].as<int>();
            claim.stage = row["stage"].as<int>();
            claim.incidentId = row["incident_id"].as<int64_t>();
            claim.encounterId = row["encounter_id"].as<int64_t>();
            claim.danger = row["danger"].as<std::string>();
            claim.checkpoint = row["checkpoint"].as<std::string>();
            claim.payload = row["payload"].as<std::string>();
            claim.retryAt = leaseUntil;
            co_return claim;
          }
        }
        co_await client->execSqlCoro(TOUCH_SCHEDULED_INBOX.data(),
                                     input.delivered, input.receivedAt,
                                     input.eventId);
        claim.kind = ObservationClaimKind::Scheduled;
        claim.attempts = std::max(storedAttempts, input.delivered);
        claim.localRetries = row["local_retries"].as<int>();
        claim.stage = row["stage"].as<int>();
        claim.incidentId = row["incident_id"].as<int64_t>();
        claim.encounterId = row["encounter_id"].as<int64_t>();
        claim.danger = row["danger"].as<std::string>();
        claim.checkpoint = row["checkpoint"].as<std::string>();
        claim.payload = row["payload"].as<std::string>();
        claim.retryAt = storedRetryAt;
        co_return claim;
      }
      co_await client->execSqlCoro(REVIVE_INBOX.data(), input.delivered,
                                   input.payload, input.payload, input.eventId);
      claim.kind = ObservationClaimKind::Retry;
      claim.attempts = std::max(row["attempts"].as<int>(), input.delivered);
      claim.localRetries = row["local_retries"].as<int>();
      claim.stage = row["stage"].as<int>();
      claim.incidentId = row["incident_id"].as<int64_t>();
      claim.encounterId = row["encounter_id"].as<int64_t>();
      claim.danger = row["danger"].as<std::string>();
      claim.checkpoint = row["checkpoint"].as<std::string>();
      claim.payload = input.payload.empty() ? row["payload"].as<std::string>()
                                            : input.payload;
      claim.retryAt = row["retry_at"].as<int64_t>();
      co_return claim;
    }
    claim.kind = ObservationClaimKind::Completed;
    co_return claim;
  }
  const auto inserted = co_await client->execSqlCoro(
      INSERT_INBOX.data(), input.eventId, input.cameraId, input.observationId,
      input.receivedAt, input.payload);
  claim.kind = inserted.affectedRows() > 0 ? ObservationClaimKind::New
                                           : ObservationClaimKind::Retry;
  claim.attempts = std::max(1, input.delivered);
  claim.payload = input.payload;
  co_return claim;
}

drogon::Task<bool> GuardRepository::setInboxRetry(
    const GuardInboxRetryInput& input) const
{
  if (input.eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      SET_INBOX_RETRY.data(), input.retryAt, input.payload, input.at,
      input.eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<GuardInboxDueRow>>
GuardRepository::dueObservations(int64_t now) const
{
  auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(DUE_INBOX.data(), now);
  std::vector<GuardInboxDueRow> due;
  due.reserve(rows.size());
  for (const auto& row : rows)
    due.push_back({.eventId = row["event_id"].as<std::string>(),
                   .payload = row["payload"].as<std::string>()});
  co_return due;
}

drogon::Task<bool> GuardRepository::claimInboxRetry(
    const GuardInboxRetryClaimInput& input) const
{
  if (input.eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      CLAIM_INBOX_RETRY.data(), input.leaseUntil, input.eventId, input.now);
  co_return result.affectedRows() > 0;
}

drogon::Task<GuardIncidentPhaseResult>
GuardRepository::commitIncidentPhase(const GuardIncidentPhaseInput& input) const
{
  GuardIncidentPhaseResult result;
  if (input.incident.eventId.empty())
    co_return result;
  auto client = DbService::client();
  if (!client)
    co_return result;

  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await client->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception&) {
    co_return result;
  }

  try {
    const auto inserted =
        co_await transaction
            ->execSqlCoro(INSERT_INCIDENT_EVENT.data(), input.incident.cameraId,
                          input.incident.cameraName, input.incident.rule,
                          input.incident.danger, input.incident.severity,
                          input.incident.personId, input.incident.identity,
                          input.incident.eventId, input.incident.eventJson,
                          input.incident.createdAt);
    result.incidentId = inserted.insertId();
    if (result.incidentId == 0) {
      const auto existing =
          co_await transaction->execSqlCoro(SELECT_INCIDENT_BY_EVENT.data(),
                                            input.incident.eventId);
      result.incidentId =
          existing.empty() ? 0 : existing.front()["id"].as<int64_t>();
    }
    if (input.consumeGuestId > 0)
      co_await transaction->execSqlCoro(CONSUME_GUEST.data(),
                                        input.consumeGuestAt,
                                        input.consumeGuestId);
    co_await transaction->execSqlCoro(ADVANCE_INBOX.data(), input.advance.stage,
                                      result.incidentId,
                                      input.advance.encounterId,
                                      input.advance.danger,
                                      input.advance.checkpoint,
                                      input.advance.at, input.advance.eventId);
  }
  catch (const std::exception& e) {
    transaction->rollback();
    LOG_WARN << "Guard incident phase failed: " << e.what();
    co_return result;
  }

  result.committed = co_await TransactionCommitAwaiter(std::move(transaction));
  co_return result;
}

drogon::Task<GuardEncounterPhaseResult> GuardRepository::commitEncounterPhase(
    const GuardEncounterPhaseInput& input) const
{
  GuardEncounterPhaseResult result;
  auto client = DbService::client();
  if (!client)
    co_return result;

  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await client->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception&) {
    co_return result;
  }

  try {
    if (input.create) {
      const auto inserted =
          co_await transaction->execSqlCoro(INSERT_ENCOUNTER.data(),
                                            input.createInput.personId,
                                            input.createInput.signature,
                                            input.createInput.bestCameraId,
                                            input.createInput.bestScore,
                                            input.createInput.at,
                                            input.createInput.at);
      result.encounterId = inserted.insertId();
      result.encounterChecks = 1;
    }
    else if (input.encounterIdToTouch > 0) {
      const auto touched =
          co_await transaction->execSqlCoro(TOUCH_ENCOUNTER.data(),
                                            input.touchInput.bestCameraId,
                                            input.touchInput.bestScore,
                                            input.touchInput.lastSeen,
                                            input.encounterIdToTouch);
      result.encounterId = input.encounterIdToTouch;
      result.encounterChecks =
          touched.empty() ? 0 : touched.front()["checks"].as<int>();
    }

    if (input.closeForPerson && input.closePersonId > 0) {
      const auto rows =
          co_await transaction->execSqlCoro(ENCOUNTERS_FOR_PERSON.data(),
                                            input.closePersonId);
      for (const auto& row : rows) {
        const GuardEncounter encounter = encounterFromRow(row);
        result.closed.push_back(encounter);
        co_await transaction->execSqlCoro(INSERT_ENCOUNTER_OUTBOX.data(),
                                          encounterClosedEventId(encounter,
                                                                 input.closeAt),
                                          encounterClosedPayload(encounter,
                                                                 input.closeAt),
                                          input.closeAt, input.closeAt);
      }
      co_await transaction->execSqlCoro(CLOSE_PERSON_TRANSITIONS.data(),
                                        input.closeAt, input.closePersonId);
      co_await transaction->execSqlCoro(CLOSE_ENCOUNTERS_FOR_PERSON.data(),
                                        input.closeAt, input.closePersonId);
    }

    Json::Value checkpoint = json_util::fromString(input.advance.checkpoint);
    if (!checkpoint.isObject())
      checkpoint = Json::Value(Json::objectValue);
    checkpoint["encounterChecks"] = result.encounterChecks;
    co_await transaction->execSqlCoro(ADVANCE_INBOX.data(), input.advance.stage,
                                      input.advance.incidentId,
                                      result.encounterId, input.advance.danger,
                                      json_util::toString(checkpoint),
                                      input.advance.at, input.advance.eventId);
  }
  catch (const std::exception& e) {
    transaction->rollback();
    LOG_WARN << "Guard encounter phase failed: " << e.what();
    co_return result;
  }

  result.committed = co_await TransactionCommitAwaiter(std::move(transaction));
  co_return result;
}

drogon::Task<bool>
GuardRepository::markObservationDeadLettered(const std::string& eventId,
                                             int64_t at) const
{
  if (eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(DEAD_LETTER_INBOX.data(), at, eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> GuardRepository::advanceObservation(
    const GuardObservationAdvanceInput& input) const
{
  if (input.eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(ADVANCE_INBOX.data(), input.stage,
                                   input.incidentId, input.encounterId,
                                   input.danger, input.checkpoint, input.at,
                                   input.eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::setDialogueGoal(const GuardDialogueGoalInput& input) const
{
  if (input.encounterId <= 0)
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(SET_DIALOGUE_GOAL.data(), input.goal,
                                   input.listeningUntil, input.lastLine,
                                   input.lastHeard, input.encounterId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::parkObservation(const GuardParkInput& input) const
{
  if (input.letter.eventId.empty())
    co_return false;
  auto client = DbService::client();
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await client->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception&) {
    co_return false;
  }
  try {
    co_await transaction->execSqlCoro(INSERT_DEAD_LETTER.data(),
                                      input.letter.eventId,
                                      input.letter.payload, input.letter.reason,
                                      input.letter.attempts, input.letter.at);
    co_await transaction->execSqlCoro(DEAD_LETTER_INBOX.data(), input.at,
                                      input.letter.eventId);
  }
  catch (const std::exception& e) {
    transaction->rollback();
    LOG_WARN << "Guard park phase failed: " << e.what();
    co_return false;
  }
  co_return co_await TransactionCommitAwaiter(std::move(transaction));
}

drogon::Task<bool>
GuardRepository::insertDeadLetter(const GuardDeadLetterInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_DEAD_LETTER.data(), input.eventId,
                                   input.payload, input.reason, input.attempts,
                                   input.at);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::completeObservation(const std::string& eventId,
                                     int64_t at) const
{
  if (eventId.empty())
    co_return true;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(COMPLETE_INBOX.data(), at, eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::optional<GuardIntent>>
GuardRepository::planIntent(const GuardOutboxInput& input) const
{
  if (input.commandId.empty())
    co_return std::nullopt;
  auto client = DbService::client();
  co_await client->execSqlCoro(INSERT_OUTBOX.data(), input.commandId,
                               input.encounterId, input.incidentId,
                               input.cameraId, input.personId, input.kind,
                               input.payload, input.at, input.at);
  const auto row =
      co_await client->execSqlCoro(SELECT_OUTBOX.data(), input.commandId);
  if (row.empty())
    co_return std::nullopt;
  const auto status = guardIntentStatusFromString(
      row.front()["status"].as<std::string>());
  if (!status.has_value()) {
    LOG_WARN << "Guard outbox: unknown persisted status for command "
             << input.commandId << "; refusing to replay it";
    co_return std::nullopt;
  }
  co_return GuardIntent{.status = *status,
                        .detail = row.front()["detail"].as<std::string>(),
                        .response = row.front()["response"].as<std::string>(),
                        .payload = row.front()["payload"].as<std::string>(),
                        .attempts = row.front()["attempts"].as<int>(),
                        .nextAttemptAt =
                            row.front()["next_attempt_at"].as<int64_t>()};
}

drogon::Task<bool>
GuardRepository::setOutboxPayload(const GuardOutboxPayloadInput& input) const
{
  if (input.commandId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(SET_OUTBOX_PAYLOAD.data(), input.payload,
                                   input.at, input.commandId, input.payload);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::enqueueEncounterClosed(const EncounterOutboxInput& input) const
{
  if (input.eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_ENCOUNTER_OUTBOX.data(),
                                   input.eventId, input.payload, input.at,
                                   input.at);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<EncounterOutboxRow>>
GuardRepository::pendingEncounterClosed() const
{
  auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(PENDING_ENCOUNTER_OUTBOX.data());
  std::vector<EncounterOutboxRow> pending;
  pending.reserve(rows.size());
  for (const auto& row : rows)
    pending.push_back({.eventId = row["event_id"].as<std::string>(),
                       .payload = row["payload"].as<std::string>()});
  co_return pending;
}

drogon::Task<bool>
GuardRepository::markEncounterSent(const std::string& eventId, int64_t at) const
{
  if (eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(MARK_ENCOUNTER_SENT.data(), at, eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::recordEncounterAttempt(const std::string& eventId,
                                        int64_t at) const
{
  if (eventId.empty())
    co_return false;
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(RECORD_ENCOUNTER_ATTEMPT.data(), at,
                                   eventId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
GuardRepository::updateOutbox(const GuardOutboxUpdateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(UPDATE_OUTBOX.data(),
                                   guardIntentStatusToString(input.status),
                                   input.detail, input.response,
                                   input.nextAttemptAt, input.at,
                                   input.commandId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> GuardRepository::markEvidenceDeleted(int64_t id,
                                                        int64_t at) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(MARK_EVIDENCE_DELETED.data(), at, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
GuardRepository::insertEvidence(const GuardEvidenceInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT_EVIDENCE.data(), input.incidentId,
                                   input.encounterId, input.cameraId,
                                   input.objectKey, input.contentType,
                                   input.retentionClass, input.createdAt,
                                   input.expiresAt);
  co_return result.insertId();
}

drogon::Task<std::vector<GuardEvidenceRow>>
GuardRepository::expiredEvidence(int64_t at) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(EXPIRED_EVIDENCE.data(), at);
  std::vector<GuardEvidenceRow> rows;
  rows.reserve(result.size());
  for (const auto& row : result) {
    rows.push_back({.id = row["id"].as<int64_t>(),
                    .objectKey = row["object_key"].as<std::string>()});
  }
  co_return rows;
}

drogon::Task<bool>
GuardRepository::canActEncounter(const GuardEncounterActionInput& input) const
{
  if (input.id <= 0)
    co_return false;
  auto client = DbService::client();
  const auto last =
      co_await client->execSqlCoro(ENCOUNTER_LAST_ACTION.data(), input.id);
  if (!last.empty()) {
    const int64_t at = last.front()["last_action_at"].as<int64_t>();
    if (input.now - at < input.cooldownS)
      co_return false;
  }
  if (input.maxPerHour > 0) {
    const auto count =
        co_await client->execSqlCoro(COUNT_ENCOUNTER_ACTIONS_SINCE.data(),
                                     input.id, input.now - 3600);
    if (!count.empty() &&
        count.front()["total"].as<int64_t>() >= input.maxPerHour)
      co_return false;
  }
  const auto marked = co_await client->execSqlCoro(MARK_ENCOUNTER_ACTION.data(),
                                                   input.now, input.id);
  co_return marked.affectedRows() > 0;
}

drogon::Task<bool> GuardRepository::closeStaleEncounters(
    const GuardStaleEncountersInput& input) const
{
  auto client = DbService::client();
  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = co_await client->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception&) {
    co_return false;
  }
  try {
    co_await transaction->execSqlCoro(CLOSE_STALE_TRANSITIONS.data(),
                                      input.olderThan, input.olderThan);
    const auto closed =
        co_await transaction->execSqlCoro(CLOSE_STALE_ENCOUNTERS.data(),
                                          input.olderThan);
    for (const auto& row : closed) {
      GuardEncounter encounter;
      encounter.id = row["id"].as<int64_t>();
      encounter.personId = row["person_id"].as<int64_t>();
      encounter.grade = row["grade"].as<std::string>();
      encounter.bestCameraId = row["best_camera_id"].as<int64_t>();
      encounter.firstSeen = row["first_seen"].as<int64_t>();
      encounter.lastSeen = row["last_seen"].as<int64_t>();
      co_await transaction->execSqlCoro(INSERT_ENCOUNTER_OUTBOX.data(),
                                        encounterClosedEventId(encounter,
                                                               input.closedAt),
                                        encounterClosedPayload(encounter,
                                                               input.closedAt),
                                        input.closedAt, input.closedAt);
    }
  }
  catch (const std::exception& e) {
    transaction->rollback();
    LOG_WARN << "Guard stale encounter close failed: " << e.what();
    co_return false;
  }
  co_return co_await TransactionCommitAwaiter(std::move(transaction));
}

drogon::Task<bool> GuardRepository::closeEncountersForPerson(int64_t personId,
                                                             int64_t at) const
{
  if (personId <= 0)
    co_return false;
  auto client = DbService::client();
  co_await client->execSqlCoro(CLOSE_PERSON_TRANSITIONS.data(), at, personId);
  const auto result =
      co_await client->execSqlCoro(CLOSE_ENCOUNTERS_FOR_PERSON.data(), at,
                                   personId);
  co_return result.affectedRows() > 0;
}
