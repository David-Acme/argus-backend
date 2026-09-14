#pragma once

#include "guard-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

class GuardRepository
{
public:
  GuardRepository() = default;
  ~GuardRepository() = default;

  drogon::Task<int64_t> insertIncident(const GuardIncidentInput& input) const;

  drogon::Task<int64_t>
  insertIncidentForEvent(const GuardIncidentInput& input) const;

  drogon::Task<bool> updateIncidentDanger(int64_t id,
                                          const std::string& danger) const;

  drogon::Task<int64_t> insertAction(const GuardActionInput& input) const;

  drogon::Task<int> repeatCount(int64_t personId, int64_t since) const;

  drogon::Task<bool> canAct(const GuardCanActInput& input) const;

  drogon::Task<int64_t> effectsSince(int64_t cameraId, int64_t since) const;

  drogon::Task<std::string> state(const std::string& key,
                                  const std::string& fallback) const;

  drogon::Task<bool> setState(const GuardStateInput& input) const;

  drogon::Task<std::vector<Json::Value>> recentIncidents(int limit) const;

  drogon::Task<int64_t> insertGuest(const GuardGuestInput& input) const;

  drogon::Task<std::optional<GuardGuest>> activeGuest(int64_t at,
                                                      int64_t cameraId) const;

  drogon::Task<bool> consumeGuest(int64_t id, int64_t at) const;

  drogon::Task<std::vector<GuardGuest>> listGuests() const;

  drogon::Task<bool> removeGuest(int64_t id) const;

  drogon::Task<int64_t>
  insertAssessment(const GuardAssessmentRowInput& input) const;

  drogon::Task<std::vector<GuardEncounter>> openEncounters(int64_t since) const;

  drogon::Task<std::vector<GuardEncounter>>
  staleEncounters(int64_t olderThan) const;

  drogon::Task<std::vector<GuardEncounter>>
  encountersForPerson(int64_t personId) const;

  drogon::Task<int64_t>
  createEncounter(const GuardEncounterCreateInput& input) const;

  drogon::Task<int> touchEncounter(const GuardEncounterTouchInput& input) const;

  drogon::Task<bool>
  transitionEncounter(const GuardTransitionInput& input) const;

  drogon::Task<ObservationClaim>
  claimObservation(const GuardInboxInput& input) const;

  drogon::Task<bool> completeObservation(const std::string& eventId,
                                         int64_t at) const;

  drogon::Task<bool> setInboxRetry(const GuardInboxRetryInput& input) const;

  drogon::Task<std::vector<GuardInboxDueRow>> dueObservations(
      int64_t now) const;

  drogon::Task<bool> claimInboxRetry(
      const GuardInboxRetryClaimInput& input) const;

  drogon::Task<bool> markObservationDeadLettered(const std::string& eventId,
                                                 int64_t at) const;

  drogon::Task<bool>
  advanceObservation(const GuardObservationAdvanceInput& input) const;

  drogon::Task<GuardIncidentPhaseResult>
  commitIncidentPhase(const GuardIncidentPhaseInput& input) const;

  drogon::Task<GuardEncounterPhaseResult>
  commitEncounterPhase(const GuardEncounterPhaseInput& input) const;

  drogon::Task<std::optional<GuardIntent>>
  planIntent(const GuardOutboxInput& input) const;

  drogon::Task<bool> updateOutbox(const GuardOutboxUpdateInput& input) const;

  drogon::Task<bool>
  setOutboxPayload(const GuardOutboxPayloadInput& input) const;

  drogon::Task<bool>
  enqueueEncounterClosed(const EncounterOutboxInput& input) const;

  drogon::Task<std::vector<EncounterOutboxRow>> pendingEncounterClosed() const;

  drogon::Task<bool> markEncounterSent(const std::string& eventId,
                                       int64_t at) const;

  drogon::Task<bool> recordEncounterAttempt(const std::string& eventId,
                                            int64_t at) const;

  drogon::Task<bool> insertDeadLetter(const GuardDeadLetterInput& input) const;

  drogon::Task<bool> recordDialogue(const GuardDialogueInput& input) const;

  drogon::Task<bool> setDialogueGoal(const GuardDialogueGoalInput& input) const;

  drogon::Task<bool> parkObservation(const GuardParkInput& input) const;

  drogon::Task<std::optional<GuardEncounter>>
  findEncounter(int64_t encounterId) const;

  drogon::Task<int64_t> insertEvidence(const GuardEvidenceInput& input) const;

  drogon::Task<std::vector<GuardEvidenceRow>> expiredEvidence(int64_t at) const;

  drogon::Task<bool> markEvidenceDeleted(int64_t id, int64_t at) const;

  drogon::Task<bool>
  canActEncounter(const GuardEncounterActionInput& input) const;

  drogon::Task<bool>
  closeStaleEncounters(const GuardStaleEncountersInput& input) const;

  drogon::Task<bool> closeEncountersForPerson(int64_t personId,
                                              int64_t at) const;
};
