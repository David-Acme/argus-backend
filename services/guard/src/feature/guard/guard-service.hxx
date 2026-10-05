#pragma once

#include "guard-action.hxx"
#include "guard-assessment.hxx"
#include "guard-belief.hxx"
#include "guard-context.hxx"
#include "guard-copy.hxx"
#include "guard-policy.hxx"
#include "guard-repository.hxx"
#include "guard-schedule.hxx"

#include <feature/guard/repositories/camera-context/camera-context-repository.hxx>
#include <feature/guard/repositories/episode/episode-repository.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <feature/guard/repositories/response/response-repository.hxx>
#include <feature/guard/services/response-plan.hxx>
#include <shared/repositories/presence/presence-repository.hxx>

#include <atomic>
#include <config/guard-config.hxx>
#include <cstdint>
#include <ctime>
#include <deque>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <map>
#include <memory>
#include <mutex>
#include <nats/nats-subject.hxx>
#include <optional>
#include <storage/s3-storage-service.hxx>
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

class GuardService
{
public:
  struct SafetyAlertInput
  {
    bool duress{false};
    int64_t alertId{0};
    int64_t actorUserId{0};
    std::string actorName;
    int64_t environmentId{0};
    int64_t now{0};
  };

  [[nodiscard]] drogon::Task<bool> raiseSafetyAlert(const SafetyAlertInput& input);

  struct Dependencies
  {
    NatsBus* bus{nullptr};
    IdentityClient* identity{nullptr};
    NotificationClient* notifications{nullptr};
    CameraActionClient* actions{nullptr};
    GuardAssessment* assessment{nullptr};
    std::shared_ptr<const ResponseDirectory> directory{};
  };

  using Config = GuardServiceConfig;

  GuardService(Dependencies dependencies, Config config);
  ~GuardService();

  void start();

  [[nodiscard]] std::shared_ptr<const Config> currentConfig() const;

  void refresh(const Config& fresh);

  void requestStop();

  [[nodiscard]] bool drained() const;

  drogon::Task<bool> handle(const Json::Value& event, int delivered);

  drogon::Task<bool> handleLocalRetry(const Json::Value& event);

  void ingestHealth(int64_t cameraId, const std::string& status, int64_t atMs);

  drogon::Task<void> checkTamperSweep(int64_t now);

  drogon::Task<void> flushEncounterOutbox();

  drogon::Task<void> maybeSendDigests(int64_t now);

private:
  struct QueueEntry
  {
    std::string payload;
    std::function<void()> ack{};
    std::function<void()> nak{};
    std::function<void()> term{};
    int delivered{0};
    bool leased{false};
  };

  bool trySubscribe();

  [[nodiscard]] bool ensureGuardStream() const;

  void scheduleSubscribeRetry();

  void subscribeAdvisories();

  std::string sanitizeSpoken(const std::string& text) const;

  void failAt(const std::string& name) const;

  void startSweeps();

  struct NotifyContent
  {
    GuardNotice notice;
    Json::Value data;
    std::vector<int64_t> excludeUserIds;
  };

  struct NotifyInput
  {
    std::string commandId;
    std::string eventId;
    std::string payload;
    NotifyContent content;
    int64_t cameraId{0};
    GuardDanger danger{GuardDanger::None};
    int64_t encounterId{0};
    int64_t now{0};
  };

  struct RecipientBatch
  {
    std::string lang;
    std::vector<int64_t> userIds;
  };

  struct RecipientQuery
  {
    const Json::Value& data;
    const std::vector<int64_t>& excluded;
    int64_t now{0};
  };

  struct RecipientResolution
  {
    std::vector<RecipientBatch> batches;
    Json::Value plan;
  };

  drogon::Task<std::optional<RecipientResolution>>
  recipientsFor(const RecipientQuery& query);

  drogon::Task<std::optional<RecipientResolution>>
  legacyRecipients(const std::vector<int64_t>& excluded);

  struct PlanQuery
  {
    const Json::Value& data;
    const std::vector<int64_t>& excluded;
    const std::vector<ResponseUser>& users;
    int64_t now{0};
  };

  drogon::Task<RecipientResolution> planRecipients(const PlanQuery& query);

  drogon::Task<bool> familyInside(int64_t environmentId);

  std::string userLang(int64_t userId);

  struct EffectInput
  {
    GuardActionKind kind{GuardActionKind::Notify};
    GuardDanger danger{GuardDanger::None};
    bool greetingEnabled{false};
    bool replyRequested{false};
    int64_t cameraId{0};
    int64_t incidentId{0};
    int64_t encounterId{0};
    int64_t personId{0};
    int64_t now{0};
    std::string text{};
    std::string lang{};
    int seconds{0};
    std::string correlationId;
    int sequence{0};
    NotifyContent notifyContent;
  };

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

  drogon::Task<ObservationResult>
  applyObservation(const ObservationInput& input);

  struct ObservationCheckpoint
  {
    int encounterChecks{0};
    bool areaInUse{false};
    bool passerby{false};
    bool expectedArea{false};
    std::string cameraRole;
    bool outdoor{false};
    bool familyInside{false};
    GuardVisitor visitor;
    std::vector<std::string> reasons;
    bool holdsComputed{false};
    bool quietHold{false};
    bool budgetHold{false};
    int64_t groupedInto{-1};
    int visitCount{0};
    bool expectedGuest{false};
    int64_t guestId{0};
    bool guestOneTime{false};
    bool hardFloor{false};
    bool observedOnly{false};
    bool effectsDenied{false};
    bool effectsStarted{false};
    bool weapon{false};
    bool lateAssessed{false};
    bool lateRaised{false};
    std::string policyDanger;
    std::string danger;
    std::string greetingStatus;
    std::string greetingDetail;
    DialogueResult dialogue;
    GuardAssessmentResult assessment;
  };

  struct AssessIntoInput
  {
    const GuardEventSignals& signals;
    ObservationCheckpoint& checkpoint;
    GuardDanger danger{GuardDanger::None};
    bool hardFloor{false};
    int& assessMs;
    std::string profile;
  };

  drogon::Task<GuardDanger> assessInto(const AssessIntoInput& input);

  struct RecordAssessmentInput
  {
    const GuardEventSignals& signals;
    const ObservationCheckpoint& checkpoint;
    int64_t incidentId{0};
    int64_t encounterId{0};
    std::string eventId;
    int64_t now{0};
  };

  drogon::Task<void> recordAssessment(const RecordAssessmentInput& input);

  struct EscalateInput
  {
    const GuardEventSignals& signals;
    const ObservationCheckpoint& checkpoint;
    const GuardPosture& posture;
    const GuardEnvironmentScope& scope;
    GuardDanger danger{GuardDanger::None};
    int64_t incidentId{0};
    int64_t encounterId{0};
    std::string eventId;
    int64_t now{0};
  };

  struct EscalationResult
  {
    bool resumable{false};
    int64_t retryAt{0};
  };

  drogon::Task<EscalationResult> escalate(const EscalateInput& input);

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
    std::vector<std::string> reasons;
    double noveltyScore{0.0};
    int repeatVisits{0};
    bool quietHold{false};
    bool budgetHold{false};
    int assessMs{0};
    int64_t environmentId{0};
    int64_t at{0};
  };

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

  struct HoldInput
  {
    const GuardEnvironment& environment;
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

  drogon::Task<bool> advanceObservation(const AdvanceInput& input);

  void subscribeHealth();

  std::string cameraHealthStatus(int64_t cameraId) const;

  drogon::Task<bool> journalDecision(const JournalDecisionInput& input);

  BeliefConfig beliefConfig(int64_t cameraId) const;

  void startHeartbeat();

  drogon::Task<void> reconcileObservations();

  int64_t retryBackoffAt(int attempts, int64_t now) const;

  void enqueue(QueueEntry entry);

  drogon::Task<void> processQueue(int64_t cameraId);

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

  void publishHeartbeat();

  struct KnownSeenInput
  {
    std::string eventId;
    int64_t personId{0};
    int64_t cameraId{0};
    std::string cameraName;
    int64_t environmentId{0};
    std::string environmentName;
    int64_t at{0};
    bool passerby{false};
  };

  void publishKnownSeen(const KnownSeenInput& input);

  struct PostureInput
  {
    const GuardSchedule& schedule;
    GuardMode manual{GuardMode::Home};
    int64_t now{0};
  };

  static GuardPosture postureAt(const PostureInput& input);

  drogon::Task<GuardEnvironmentScope> environmentScope(int64_t cameraId) const;

  void rememberCameraName(int64_t cameraId, const std::string& name);

  std::string cameraName(int64_t cameraId) const;

  struct DigestInput
  {
    const GuardEnvironmentScope& scope;
    std::string day;
    int64_t from{0};
    int64_t to{0};
    bool afterQuiet{false};
    std::string correlationId;
    int64_t now{0};
  };

  drogon::Task<bool> sendDigest(const DigestInput& input);
  struct EnvironmentDigestInput
  {
    GuardEnvironmentScope scope;
    const Config& config;
    std::tm local{};
    int64_t now{0};
  };
  drogon::Task<void> sendEnvironmentDigests(const EnvironmentDigestInput& input);

  void trackTimer(uint64_t id);
  void stopTimers();

  Dependencies dependencies_;
  mutable std::mutex configMutex_;
  std::shared_ptr<const Config> config_;
  GuardRepository repository_;
  EnvironmentRepository environmentRepository_;
  CameraContextRepository cameraContextRepository_;
  ResponseRepository responseRepository_;
  PresenceRepository presenceRepository_;
  EpisodeRepository episodeRepository_;
  S3StorageService storage_;
  std::shared_ptr<GuardLifecycle> lifecycle_;

  struct QueueLane
  {
    std::deque<QueueEntry> entries;
    bool processing{false};
  };

  std::mutex queueMutex_;
  std::map<int64_t, QueueLane> lanes_;

  std::mutex lifecycleMutex_;
  std::vector<uint64_t> timerIds_;
  std::unordered_set<std::string> executing_;

  bool subscribed_{false};
  std::atomic<bool> encounterStreamReady_{false};
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
  std::map<int64_t, std::string> cameraNames_;

  struct LangEntry
  {
    std::string lang;
    int64_t resolvedAt{0};
  };
  std::mutex langMutex_;
  std::map<int64_t, LangEntry> langCache_;

  struct BeliefCacheEntry
  {
    BeliefConfig config;
    int64_t resolvedAt{0};
  };
  mutable std::mutex beliefMutex_;
  mutable std::map<int64_t, BeliefCacheEntry> beliefCache_;
};
