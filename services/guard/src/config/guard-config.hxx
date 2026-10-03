#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>
#include <nats/nats-subject.hxx>
#include <shared/vocabulary/belief-config.hxx>
#include <shared/vocabulary/belief-gate-scope.hxx>
#include <shared/vocabulary/guard-mode.hxx>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GuardDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct GuardRpcConfig
{
  std::string address;
  std::vector<argus::client::CallerCredential> settingsCredentials;
};

struct GuardPeerConfig
{
  std::string target;
  std::string secret;
};

struct GuardAssessEndpoints
{
  std::string vlmUrl;
  std::string vlmTarget;
  std::string llmUrl;
  std::string llmTarget;
  int timeoutMs{8000};
};

struct GuardAssessmentConfig
{
  bool enabled{false};
  std::string mode{"agent"};
  std::string vetoScope{"soft_only"};
  int maxToolRounds{3};
  int maxAnnounceWords{12};
  std::string lang{"es"};
};

struct GuardScheduleConfig
{
  bool enabled{false};
  std::string asleep;
  std::string open;
  std::string staffed;
  std::string closedMode{"away"};
};

struct GuardServiceConfig
{
  bool enabled{false};
  std::string profile{"home"};
  GuardMode defaultMode{GuardMode::Home};
  GuardScheduleConfig schedule;
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
  int64_t continuityWindowS{45};
  double signatureMinSimilarity{0.82};
  int loiterChecks{3};
  bool stagingEnabled{true};
  int64_t encounterTimeoutS{300};
  int heartbeatS{10};
  int maxDialogueTurns{3};
  int maxObservationAttempts{5};
  int retryBaseMs{1000};
  int retryMaxMs{60000};
  int64_t retryLeaseMs{60000};
  int maxAnnounceWords{12};
  std::string consumerDurable{"argus-guard"};
  std::string eventStream{"ARGUS_CAMERA"};
  std::string eventSubject;
  std::string guardStream{nats_subject::kGuardStream};
  std::string guardSubjectFilter;
  std::string guardEncounterSubject;
  std::string decisionMode{"shadow"};
  BeliefGateScope beliefGateScope{BeliefGateScope::Notify};
  int64_t beliefRefreshS{300};
  int journalRetentionDays{90};
  bool quietHoursEnabled{false};
  int quietStartHour{22};
  int quietEndHour{7};
  int quietDailyBudget{30};
  int digestHour{21};
  int64_t regroupWindowS{600};
  std::string notifyLang{"es"};
  int64_t tamperSustainedS{300};
  int64_t healthStaleS{300};
  std::function<bool(const std::string&)> failPoint;
};

class GuardConfig
{
public:
  [[nodiscard]] static GuardDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static GuardRpcConfig resolveRpc();

  [[nodiscard]] static GuardPeerConfig resolveNotifications();

  [[nodiscard]] static GuardPeerConfig resolveIdentity();

  [[nodiscard]] static GuardPeerConfig resolveActions();

  [[nodiscard]] static GuardAssessEndpoints resolveAssessEndpoints();

  [[nodiscard]] static GuardAssessmentConfig resolveAssessment();

  [[nodiscard]] static GuardServiceConfig resolveService();

  [[nodiscard]] static BeliefConfig resolveBelief(int64_t cameraId);
};
