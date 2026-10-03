#include "guard-config.hxx"

#include <config/config-service.hxx>
#include <shared/vocabulary/guard-mode.hxx>
#include <trantor/utils/Logger.h>

#include <string>
#include <utility>
#include <vector>

namespace
{
std::string configOr(const std::string& key, const std::string& fallback)
{
  const std::string value = ConfigService::getString(key);
  return value.empty() ? fallback : value;
}

int configIntOr(const std::string& key, int fallback)
{
  const int value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}

int64_t configInt64Or(const std::string& key, int64_t fallback)
{
  const int value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}

bool configBoolOr(const std::string& key, bool fallback)
{
  return ConfigService::hasKey(key) ? ConfigService::getBool(key) : fallback;
}

double configDoubleOr(const std::string& key, double fallback)
{
  const double value = ConfigService::getDouble(key);
  return value > 0 ? value : fallback;
}

std::vector<std::string> configVariantsOr(const std::string& key,
                                          const std::string& fallback)
{
  const std::string value = configOr(key, fallback);
  std::vector<std::string> variants;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t end = value.find('|', start);
    std::string variant =
        value.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
    const size_t begin = variant.find_first_not_of(" \t");
    const size_t last = variant.find_last_not_of(" \t");
    variant = begin == std::string::npos
                  ? ""
                  : variant.substr(begin, last - begin + 1);
    if (!variant.empty())
      variants.push_back(std::move(variant));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return variants;
}

int clampHour(int hour, int fallback)
{
  return hour < 0 || hour > 23 ? fallback : hour;
}

int beliefIntOr(const std::string& key, int fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return ConfigService::getInt(key);
}

int64_t beliefInt64Or(const std::string& key, int64_t fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return static_cast<int64_t>(ConfigService::getInt(key));
}

double beliefDoubleOr(const std::string& key, double fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return ConfigService::getDouble(key);
}

std::string cameraLeafKey(int64_t cameraId, const std::string& leaf)
{
  return "guard.belief.camera." + std::to_string(cameraId) + "." + leaf;
}

std::string beliefLeafKey(int64_t cameraId, const std::string& leaf)
{
  const std::string overrideKey = cameraLeafKey(cameraId, leaf);
  if (ConfigService::hasKey(overrideKey))
    return overrideKey;
  return "guard.belief." + leaf;
}
}

GuardDbConfig GuardConfig::resolveDb()
{
  return {.dbPath = configOr("database.db", "database/guard.db"),
          .schemaPath = configOr("database.schema",
                                 "services/guard/database/schema.sql")};
}

ListenerConfig GuardConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("guard", 7039);
}

GuardPeerConfig GuardConfig::resolveNotifications()
{
  return {.target = ConfigService::getString("notifications.grpc_target"),
          .secret = ConfigService::getString("notifications.credential")};
}

GuardPeerConfig GuardConfig::resolveIdentity()
{
  return {.target = ConfigService::getString("identity.target"),
          .secret = ConfigService::getString("identity.rpc_secret")};
}

GuardPeerConfig GuardConfig::resolveActions()
{
  return {.target = ConfigService::getString("camera.actions_target"),
          .secret = ConfigService::getString("camera.actions_credential")};
}

GuardAssessEndpoints GuardConfig::resolveAssessEndpoints()
{
  return {.vlmUrl = ConfigService::getString("guard.assess.vlm_url"),
          .vlmTarget = ConfigService::getString("vlm.grpc_target"),
          .llmUrl = ConfigService::getString("guard.assess.llm_url"),
          .llmTarget = ConfigService::getString("llm.grpc_target"),
          .timeoutMs = configIntOr("guard.assess.timeout_ms", 8000)};
}

GuardAssessmentConfig GuardConfig::resolveAssessment()
{
  return {
      .enabled = ConfigService::getBool("guard.assess.enabled"),
      .mode = configOr("guard.assess.mode", "agent"),
      .vetoScope = configOr("guard.veto_scope", "soft_only"),
      .maxToolRounds = configIntOr("guard.assess.max_tool_rounds", 3),
      .maxAnnounceWords = configIntOr("guard.max_announce_words", 12),
      .lang = configOr("guard.announce_lang", "es")};
}

GuardServiceConfig GuardConfig::resolveService()
{
  GuardServiceConfig config;
  config.enabled = ConfigService::getBool("guard.enabled");
  config.profile = configOr("guard.profile", "home");
  config.defaultMode =
      guardModeFromString(configOr("guard.default_mode", "home"));
  config.schedule = {.enabled = configBoolOr("guard.schedule.enabled", false),
                     .asleep = configOr("guard.schedule.asleep", ""),
                     .open = configOr("guard.schedule.open", ""),
                     .staffed = configOr("guard.schedule.staffed", ""),
                     .closedMode =
                         configOr("guard.schedule.closed_mode", "away")};
  config.notifyLevel = configIntOr("guard.notify_level", 2);
  config.announceLevel = configIntOr("guard.announce_level", 3);
  config.alarmLevel = configIntOr("guard.alarm_level", 4);
  config.announceText =
      configOr("guard.announce_text",
               "Atención: está en una propiedad privada. El propietario ya ha "
               "sido avisado.");
  config.announceLang = configOr("guard.announce_lang", "es");
  config.greetEnabled = configBoolOr("guard.greet_enabled", true);
  config.greetKnown = configBoolOr("guard.greet_known", false);
  config.greetText = configOr("guard.greet_text", "Hola, ¿necesitas algo?");
  config.greetTexts = configVariantsOr("guard.greet_texts", "");
  if (config.greetTexts.empty() && !config.greetText.empty())
    config.greetTexts.push_back(config.greetText);
  config.greetKnownText =
      configOr("guard.greet_known_text", "Hola {name}, ¿necesitas algo?");
  config.greetLang = configOr("guard.greet_lang", config.announceLang);
  config.greetListenSeconds = configIntOr("guard.greet_listen_seconds", 6);
  config.greetReplyEnabled = configBoolOr("guard.greet_reply_enabled", true);
  config.greetReplyText =
      configOr("guard.greet_reply_text", "Perfecto, dime, ¿en qué te ayudo?");
  config.greetReplyTexts = configVariantsOr("guard.greet_reply_texts", "");
  config.greetReplyLang = configOr("guard.greet_reply_lang", config.greetLang);
  config.greetRepairText = configOr("guard.greet_repair_text",
                                    "No te escuché bien. ¿Puedes repetirlo?");
  config.alarmSeconds = configIntOr("guard.alarm_seconds", 6);
  config.armSiren = configBoolOr("guard.arm_siren", false);
  config.sirenSeconds = configIntOr("guard.siren_seconds", 20);
  config.vetoScope = configOr("guard.veto_scope", "soft_only");
  config.actionCooldownS = configInt64Or("guard.action_cooldown_s", 120);
  config.repeatWindowS = configInt64Or("guard.repeat_window_s", 86400);
  config.maxActionsPerHour = configInt64Or("guard.max_actions_per_hour", 4);
  config.expectedGuestsEnabled = configBoolOr("guard.expected_guests", true);
  config.crossCameraWindowS = configInt64Or("guard.cross_camera_window_s", 60);
  config.continuityWindowS = configInt64Or("guard.continuity_window_s", 45);
  config.signatureMinSimilarity =
      configDoubleOr("guard.signature_min_similarity", 0.82);
  config.loiterChecks = configIntOr("guard.loiter_checks", 3);
  config.stagingEnabled = configBoolOr("guard.staging", true);
  config.encounterTimeoutS = configInt64Or("guard.encounter_timeout_s", 300);
  config.heartbeatS = configIntOr("guard.heartbeat_s", 10);
  config.maxDialogueTurns = configIntOr("guard.max_dialogue_turns", 3);
  config.maxObservationAttempts =
      configIntOr("guard.max_observation_attempts", 5);
  config.maxAnnounceWords = configIntOr("guard.max_announce_words", 12);
  config.consumerDurable = configOr("guard.consumer_durable", "argus-guard");
  config.eventStream = configOr("guard.event_stream", "ARGUS_CAMERA");
  config.eventSubject = ConfigService::getString("guard.event_subject");
  config.decisionMode = configOr("guard.decision_mode", "shadow");
  if (config.decisionMode != "shadow" && config.decisionMode != "enforce") {
    LOG_WARN << "Unknown guard.decision_mode '" << config.decisionMode
             << "'; using shadow";
    config.decisionMode = "shadow";
  }
  const auto gateScope =
      beliefGateScopeFromString(configOr("guard.belief.gate_scope", "notify"));
  if (!gateScope) {
    LOG_WARN << "Unknown guard.belief.gate_scope '"
             << configOr("guard.belief.gate_scope", "notify")
             << "'; using notify";
    config.beliefGateScope = BeliefGateScope::Notify;
  }
  else {
    config.beliefGateScope = *gateScope;
  }
  config.healthStaleS = configInt64Or("guard.health_stale_s", 300);
  config.beliefRefreshS = configInt64Or("guard.belief_refresh_s", 300);
  config.journalRetentionDays =
      configIntOr("guard.journal_retention_days", 90);
  config.quietHoursEnabled = configBoolOr("guard.quiet_hours.enabled", false);
  config.quietStartHour =
      clampHour(configIntOr("guard.quiet_hours.start_hour", 22), 22);
  config.quietEndHour =
      clampHour(configIntOr("guard.quiet_hours.end_hour", 7), 7);
  config.quietDailyBudget =
      configIntOr("guard.quiet_hours.daily_budget", 30);
  config.tamperSustainedS = configInt64Or("guard.tamper_sustained_s", 300);
  return config;
}

BeliefConfig GuardConfig::resolveBelief(int64_t cameraId)
{
  const BeliefConfig defaults;
  BeliefConfig config;
  config.weightDetectorStrong =
      beliefIntOr(beliefLeafKey(cameraId, "weight_detector_strong"),
                  defaults.weightDetectorStrong);
  config.weightDetectorWeak =
      beliefIntOr(beliefLeafKey(cameraId, "weight_detector_weak"),
                  defaults.weightDetectorWeak);
  config.weightPersistenceMet =
      beliefIntOr(beliefLeafKey(cameraId, "weight_persistence_met"),
                  defaults.weightPersistenceMet);
  config.weightPersistenceShort =
      beliefIntOr(beliefLeafKey(cameraId, "weight_persistence_short"),
                  defaults.weightPersistenceShort);
  config.weightTrackStable =
      beliefIntOr(beliefLeafKey(cameraId, "weight_track_stable"),
                  defaults.weightTrackStable);
  config.weightTrackJitter =
      beliefIntOr(beliefLeafKey(cameraId, "weight_track_jitter"),
                  defaults.weightTrackJitter);
  config.weightIdentityUnrecognized =
      beliefIntOr(beliefLeafKey(cameraId, "weight_identity_unrecognized"),
                  defaults.weightIdentityUnrecognized);
  config.weightIdentityUnobservable =
      beliefIntOr(beliefLeafKey(cameraId, "weight_identity_unobservable"),
                  defaults.weightIdentityUnobservable);
  config.weightIdentityUnavailable =
      beliefIntOr(beliefLeafKey(cameraId, "weight_identity_unavailable"),
                  defaults.weightIdentityUnavailable);
  config.weightIdentityKnown =
      beliefIntOr(beliefLeafKey(cameraId, "weight_identity_known"),
                  defaults.weightIdentityKnown);
  config.weightCameraHealthDegraded =
      beliefIntOr(beliefLeafKey(cameraId, "weight_camera_health_degraded"),
                  defaults.weightCameraHealthDegraded);
  config.detectorStrong = beliefDoubleOr(
      beliefLeafKey(cameraId, "detector_strong"), defaults.detectorStrong);
  config.detectorWeak = beliefDoubleOr(beliefLeafKey(cameraId, "detector_weak"),
                                       defaults.detectorWeak);
  config.minScoreSamples = beliefIntOr(
      beliefLeafKey(cameraId, "min_score_samples"), defaults.minScoreSamples);
  config.persistenceWindows = beliefIntOr(
      beliefLeafKey(cameraId, "persistence_windows"),
      defaults.persistenceWindows);
  config.persistenceDwellFraction = beliefDoubleOr(
      beliefLeafKey(cameraId, "persistence_dwell_fraction"),
      defaults.persistenceDwellFraction);
  config.zoneDwellAlertMs = beliefInt64Or(
      beliefLeafKey(cameraId, "zone_dwell_alert_ms"),
      defaults.zoneDwellAlertMs);
  config.zoneDwellMonitorMs = beliefInt64Or(
      beliefLeafKey(cameraId, "zone_dwell_monitor_ms"),
      defaults.zoneDwellMonitorMs);
  config.trackStableAgeMs = beliefInt64Or(
      beliefLeafKey(cameraId, "track_stable_age_ms"),
      defaults.trackStableAgeMs);
  config.trackJitterDwellMs = beliefInt64Or(
      beliefLeafKey(cameraId, "track_jitter_dwell_ms"),
      defaults.trackJitterDwellMs);
  config.areaSpreadRatio = beliefDoubleOr(
      beliefLeafKey(cameraId, "area_spread_ratio"), defaults.areaSpreadRatio);
  config.thresholdCritical =
      beliefIntOr(beliefLeafKey(cameraId, "threshold_critical"),
                  defaults.thresholdCritical);
  config.thresholdHigh = beliefIntOr(
      beliefLeafKey(cameraId, "threshold_high"), defaults.thresholdHigh);
  config.thresholdMedium = beliefIntOr(
      beliefLeafKey(cameraId, "threshold_medium"), defaults.thresholdMedium);
  config.thresholdLow = beliefIntOr(
      beliefLeafKey(cameraId, "threshold_low"), defaults.thresholdLow);
  return config;
}
