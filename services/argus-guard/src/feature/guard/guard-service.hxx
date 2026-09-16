#pragma once

#include "guard-action.hxx"
#include "guard-assessment.hxx"
#include "guard-belief.hxx"
#include "guard-policy.hxx"
#include "guard-repository.hxx"

#include <cstdint>
#include <deque>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared/services/storage/s3-storage-service.hxx>
#include <string>
#include <unordered_set>
#include <vector>

class CameraActionClient;
class GuardAssessment;
class ObservationRetryPump;
class IdentityClient;
class NotificationClient;
class NatsBus;
struct GuardLifecycle;

// Consumes object_detected, classifies danger and raises only authorized
// actions.
class GuardService
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    IdentityClient* identity{nullptr};
    NotificationClient* notifications{nullptr};
    CameraActionClient* actions{nullptr};
    GuardAssessment* assessment{nullptr};
  };

  struct Config
  {
    bool enabled{false};
    std::string profile{"home"};
    GuardMode defaultMode{GuardMode::Home};
    int notifyLevel{2};
    int announceLevel{3};
    int alarmLevel{4};
    std::string announceText;
    std::string announceLang{"es"};
    bool greetEnabled{false};
    bool greetKnown{false};
    std::string greetText;
    std::vector<std::string> greetTexts;
    std::string greetKnownText;
    std::string greetLang{"es"};
    int greetListenSeconds{0};
    bool greetReplyEnabled{true};
    std::string greetReplyText;
    std::vector<std::string> greetReplyTexts;
    std::string greetReplyLang{"es"};
    std::string greetRepairText;
    int alarmSeconds{6};
    bool armSiren{false};
    int sirenSeconds{20};
    std::string vetoScope{"soft_only"};
    int64_t actionCooldownS{120};
    int64_t repeatWindowS{86400};
    int64_t maxActionsPerHour{4};
    bool expectedGuestsEnabled{true};
    int64_t crossCameraWindowS{60};
    int64_t continuityWindowS{20};
    double signatureMinSimilarity{0.82};
    int loiterChecks{3};
    bool stagingEnabled{true};
    int64_t encounterTimeoutS{300};
    int heartbeatS{10};
    int maxDialogueTurns{3};
    int maxObservationAttempts{5};
    // Bounded retry backoff for resumable effects (ms).
    int retryBaseMs{1000};
    int retryMaxMs{60000};
    // Local-retry lease window (ms) claimed by the reconciler.
    int64_t retryLeaseMs{60000};
    int maxAnnounceWords{12};
    std::string consumerDurable{"argus-guard"};
    // JetStream source of observations; overridable for isolated live tests.
    std::string eventStream{"ARGUS_CAMERA"};
    std::string eventSubject;
    // Guard-owned JetStream stream for argus.guard.v1.* domain events.
    std::string guardStream{"ARGUS_GUARD"};
    // Belief-gate mode: "shadow" journals only, "enforce" can suppress.
    std::string decisionMode{"shadow"};
    // Effect kinds the belief gate may suppress in enforce mode.
    BeliefGateScope beliefGateScope{BeliefGateScope::Notify};
    // Belief-config refresh window; resolved per camera and cached.
    int64_t beliefRefreshS{300};
    // Decision-journal retention in days; <= 0 keeps every row.
    int journalRetentionDays{90};
    // Quiet-hours demotion markers, journal-only and default off so shadow
    // data stays clean; held rows still notify exactly as before.
    bool quietHoursEnabled{false};
    int quietStartHour{22};
    int quietEndHour{7};
    int quietDailyBudget{30};
    // Sustained-tamper escalation window; a tamper-ish health state held this
    // long raises its own notification through the durable intent path.
    int64_t tamperSustainedS{300};
    // Freshness window for camera health readings used by the belief gate.
    int64_t healthStaleS{300};
    // Deterministic test failpoints; empty in production.
    std::function<bool(const std::string&)> failPoint;
  };

  GuardService(Dependencies dependencies, Config config);
  ~GuardService();

  void start();

  // Inbound observation entry point; delivered is the JetStream delivery count.
  drogon::Task<bool> handle(const Json::Value& event, int delivered);

  // Local scheduler entry point for a retry the reconciler already leased.
  drogon::Task<bool> handleLocalRetry(const Json::Value& event);

  // Health ingestion shared by the NATS subscriber and tests.
  void ingestHealth(int64_t cameraId, const std::string& status, int64_t atMs);

  // Sustained-tamper sweep step, driven by the encounter sweep timer.
  drogon::Task<void> checkTamperSweep(int64_t now);

private:
  struct QueueEntry
  {
    std::string payload;
    std::function<void()> ack;
    std::function<void()> nak;
    std::function<void()> term;
    int delivered{0};
    bool leased{false};
  };

  bool trySubscribe();

  void scheduleSubscribeRetry();

  void subscribeAdvisories();

  // Code-side gate every spoken line must pass, configured or generated.
  std::string sanitizeSpoken(const std::string& text) const;

  void failAt(const std::string& name) const;

  void startSweeps();

  struct NotifyInput
  {
    std::string commandId;
    std::string eventId;
    std::string payload;
    std::string title;
    std::string body;
    Json::Value data;
    int64_t cameraId{0};
    std::string cameraName;
    std::string rule;
    GuardDanger danger{GuardDanger::None};
    int64_t incidentId{0};
    int64_t encounterId{0};
    int64_t now{0};
  };

  // Prebuilt notification content from persisted observables.
  struct NotifyContent
  {
    std::string title;
    std::string body;
    Json::Value data;
  };

  // One requested autonomous effect with everything authorization needs.
  struct EffectInput
  {
    GuardActionKind kind{GuardActionKind::Notify};
    GuardDanger danger{GuardDanger::None};
    bool greetingEnabled{false};
    bool replyRequested{false};
    int64_t cameraId{0};
    std::string cameraName;
    std::string rule;
    int64_t incidentId{0};
    int64_t encounterId{0};
    int64_t personId{0};
    int64_t now{0};
    std::string text;
    std::string lang;
    int seconds{0};
    std::string correlationId;
    int sequence{0};
    NotifyContent notifyContent;
  };

  // Honest effect state; a physical action that may have run is never failed.
  enum class EffectStatus : uint8_t
  {
    Succeeded = 0,
    InFlight,
    Indeterminate,
    Rejected,
    RetryableFailed
  };

  struct EffectResult
  {
    bool authorized{false};
    bool executed{false};
    bool accepted{false};
    bool pending{false};
    bool indeterminate{false};
    bool resumable{false};
    bool speechDetected{false};
    std::string detail;
    std::string heard;
    GuardIntentStatus status{GuardIntentStatus::Pending};
    int64_t retryAt{0};
  };

  struct ObservationResult
  {
    bool completed{true};
    int64_t retryAt{0};
  };

  // Dialogue eligibility resolved from the event and the encounter.
  struct DialogueInput
  {
    const GuardEventSignals& signals;
    bool knownVisit{false};
    bool unknownVisit{false};
    int64_t incidentId{0};
    int64_t encounterId{0};
    int64_t now{0};
    GuardDanger danger{GuardDanger::None};
    std::string correlationId;
  };

  struct DialogueResult
  {
    bool greeted{false};
    std::string greetingText;
    std::string greetingStatus;
    std::string greetingDetail;
    bool listened{false};
    std::string heardText;
    bool replied{false};
    std::string replyText;
    bool repairPending{false};
    bool listenAfterReply{false};
    int turns{0};
    bool resumable{false};
    int64_t retryAt{0};
  };

  struct ObservationInput
  {
    const Json::Value& event;
    const GuardEventSignals& signals;
    ObservationClaim state;
  };

  // Runs the staged saga; each stage persists its checkpoint before the next. A
  // non-completed result means a resumable effect must be retried at retryAt.
  drogon::Task<ObservationResult>
  applyObservation(const ObservationInput& input);

  // Durable saga checkpoint so a redelivery resumes instead of repeating.
  struct ObservationCheckpoint
  {
    int encounterChecks{0};
    int visitCount{0};
    bool expectedGuest{false};
    int64_t guestId{0};
    bool guestOneTime{false};
    bool hardFloor{false};
    bool observedOnly{false};
    bool effectsDenied{false};
    bool effectsStarted{false};
    std::string policyDanger;
    std::string danger;
    std::string greetingStatus;
    std::string greetingDetail;
    DialogueResult dialogue;
    GuardAssessmentResult assessment;
  };

  static Json::Value checkpointToJson(const ObservationCheckpoint& checkpoint);
  static ObservationCheckpoint checkpointFromJson(const Json::Value& json);

  struct AdvanceInput
  {
    std::string eventId;
    int stage{0};
    int64_t incidentId{0};
    int64_t encounterId{0};
    const ObservationCheckpoint& checkpoint;
    int64_t at{0};
  };

  // One belief verdict for the decision journal; written at the effects
  // stage and never allowed to fail the saga.
  struct JournalDecisionInput
  {
    std::string eventId;
    int64_t encounterId{0};
    int64_t incidentId{0};
    int64_t cameraId{0};
    std::string observationId;
    GuardDanger danger{GuardDanger::None};
    bool hardFloor{false};
    int beliefScore{0};
    std::vector<std::string> beliefSignals;
    int beliefThreshold{0};
    bool legacyWouldNotify{false};
    bool beliefWouldNotify{false};
    std::string decisionMode;
    DecisionSuppression suppression{DecisionSuppression::None};
    std::vector<std::string> suppressedKinds;
    double noveltyScore{0.0};
    int repeatVisits{0};
    bool quietHold{false};
    bool budgetHold{false};
    int assessMs{0};
    int64_t at{0};
  };

  // Offline-calibration signals collected alongside the journal write; they
  // never influence the verdict and never fail the saga.
  struct CollectionInput
  {
    int64_t cameraId{0};
    std::string signature;
    bool hasUnknown{false};
    int64_t now{0};
  };

  struct CollectionResult
  {
    double noveltyScore{0.0};
    int repeatVisits{0};
  };

  drogon::Task<CollectionResult> collectSignals(const CollectionInput& input);

  // Journal-only quiet-hours and budget markers; enforcement stays off, so a
  // held row still notifies exactly as before.
  struct HoldInput
  {
    GuardDanger danger{GuardDanger::None};
    bool legacyWouldNotify{false};
    int64_t now{0};
  };

  struct HoldResult
  {
    bool quiet{false};
    bool budget{false};
  };

  drogon::Task<HoldResult> computeHolds(const HoldInput& input);

  // Persists the checkpoint and advances the durable saga stage.
  drogon::Task<bool> advanceObservation(const AdvanceInput& input);

  void subscribeHealth();

  std::string cameraHealthStatus(int64_t cameraId) const;

  drogon::Task<bool> journalDecision(const JournalDecisionInput& input);

  BeliefConfig beliefConfig(int64_t cameraId) const;

  void startHeartbeat();

  drogon::Task<void> reconcileObservations();

  int64_t retryBackoffAt(int attempts, int64_t now) const;

  void enqueue(QueueEntry entry);

  drogon::Task<void> processQueue();

  struct HandleEventInput
  {
    const Json::Value& event;
    int delivered{0};
    bool leased{false};
  };

  drogon::Task<bool> handleEvent(HandleEventInput input);

  drogon::Task<EffectStatus> notify(const NotifyInput& input);

  drogon::Task<EffectResult> performEffect(const EffectInput& input);

  drogon::Task<DialogueResult> runDialogue(const DialogueInput& input);

  struct EvidenceInput
  {
    const Json::Value& event;
    int64_t incidentId{0};
    int64_t encounterId{0};
    int64_t cameraId{0};
    std::string cameraName;
    std::string rule;
    GuardDanger danger{GuardDanger::None};
    int64_t now{0};
  };

  drogon::Task<void> uploadEvidence(const EvidenceInput& input);

  drogon::Task<void> runRetentionSweep();

  void scheduleRetentionSweep();

  void scheduleSirenDisarm(const EffectInput& input);

  void scheduleEncounterSweep();

  void publishEncounterClosed(const GuardEncounter& encounter, int64_t at);

  drogon::Task<void> flushEncounterOutbox();

  void publishHeartbeat();

  // Shared destruction quorum: every loop callback holds one, the destructor
  // flips it and waits the callbacks out, so none ever dereference a dead
  // service. Destroy only off the loop with the loop running.
  void trackTimer(uint64_t id);
  void stopTimers();

  Dependencies dependencies_;
  Config config_;
  GuardRepository repository_;
  GuardActionAuthorizer authorizer_;
  S3StorageService storage_;
  std::shared_ptr<GuardLifecycle> lifecycle_;

  std::mutex queueMutex_;
  std::deque<QueueEntry> queue_;
  bool processing_{false};

  // Guards timer ids plus the in-flight execution set below.
  std::mutex lifecycleMutex_;
  std::vector<uint64_t> timerIds_;
  std::unordered_set<std::string> executing_;

  bool subscribed_{false};
  bool advisoriesSubscribed_{false};
  bool healthSubscribed_{false};
  bool sweepsStarted_{false};
  bool heartbeatStarted_{false};
  int subscribeAttempts_{0};
  std::shared_ptr<ObservationRetryPump> retryPump_;
  std::optional<uint64_t> durableSubscription_;
  std::optional<uint64_t> advisorySubscription_;
  std::optional<uint64_t> healthSubscription_;

  struct CameraHealth
  {
    std::string status;
    int64_t firstSeenMs{0};
    int64_t lastSeenMs{0};
  };
  mutable std::mutex healthMutex_;
  std::map<int64_t, CameraHealth> healthByCamera_;

  struct BeliefCacheEntry
  {
    BeliefConfig config;
    int64_t resolvedAt{0};
  };
  mutable std::mutex beliefMutex_;
  mutable std::map<int64_t, BeliefCacheEntry> beliefCache_;
};
