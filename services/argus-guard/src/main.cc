#include <camera/camera-action-client.hxx>
#include <drogon/drogon.h>
#include <feature/api/guard/controllers/guard-controller.hxx>
#include <feature/guard/guard-assessment.hxx>
#include <feature/guard/guard-belief.hxx>
#include <feature/guard/guard-policy.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schema.hxx>
#include <feature/guard/guard-service.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/llm/remote/llm-remote.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/vision/remote/vlm-client.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <unistd.h>

#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct GuardDrogonConfig
{
  std::string dbPath;
  std::string host;
  int port{0};
};

Json::Value drogonConfig(const GuardDrogonConfig& input)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = input.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  Json::Value clients(Json::arrayValue);
  clients.append(client);
  config["db_clients"] = clients;

  Json::Value listener(Json::objectValue);
  listener["address"] = input.host;
  listener["port"] = input.port;
  listener["https"] = false;
  Json::Value listeners(Json::arrayValue);
  listeners.append(listener);
  config["listeners"] = listeners;
  return config;
}

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

bool configBoolOr(const std::string& key, bool fallback)
{
  return ConfigService::hasKey(key) ? ConfigService::getBool(key) : fallback;
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
    variant = begin == std::string::npos ? "" : variant.substr(begin, last - begin + 1);
    if (!variant.empty())
      variants.push_back(std::move(variant));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return variants;
}

void registerHealth()
{
  drogon::app().registerHandler(
      "/health",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        Json::Value body;
        body["status"] = "ok";
        body["service"] = "argus-guard";
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      {drogon::Get});
}

} // namespace

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const std::string dbPath = configOr("database.db", "database/guard.db");
  const std::string schemaPath =
      configOr("database.schema", "services/argus-guard/database/schema.sql");
  const std::string host = configOr("server.host", "127.0.0.1");
  const int port = configIntOr("server.port", 7039);

  const std::string notificationsTarget =
      ConfigService::getString("notifications.grpc_target");
  std::unique_ptr<NotificationClient> notifications;
  if (!notificationsTarget.empty())
    notifications = std::make_unique<NotificationClient>(
        NotificationClientConfig{
            .target = notificationsTarget,
            .credential = ConfigService::getString("notifications.credential")});

  const std::string fleetSecret = ConfigService::getString("identity.rpc_secret");
  const std::string identityTarget = ConfigService::getString("identity.target");
  std::unique_ptr<IdentityClient> identity;
  if (!identityTarget.empty())
    identity = std::make_unique<IdentityClient>(identityTarget, fleetSecret);

  const std::string actionsTarget = ConfigService::getString("camera.actions_target");
  std::unique_ptr<CameraActionClient> actions;
  if (!actionsTarget.empty())
    actions = std::make_unique<CameraActionClient>(
        CameraActionClientConfig{
            .target = actionsTarget,
            .credential =
                ConfigService::getString("camera.actions_credential")});

  const int assessTimeoutMs = configIntOr("guard.assess.timeout_ms", 8000);
  std::unique_ptr<VlmClient> vlm;
  const std::string vlmUrl = ConfigService::getString("guard.assess.vlm_url");
  if (!vlmUrl.empty())
    vlm = std::make_unique<VlmClient>(
        vlmUrl, static_cast<double>(assessTimeoutMs) / 1000.0);

  std::unique_ptr<LlmHttpClient> llm;
  const std::string llmUrl = ConfigService::getString("guard.assess.llm_url");
  if (!llmUrl.empty())
    llm = std::make_unique<LlmHttpClient>(llmUrl, assessTimeoutMs);

  GuardAssessment::Config assessConfig;
  assessConfig.enabled = ConfigService::getBool("guard.assess.enabled");
  assessConfig.mode = configOr("guard.assess.mode", "agent");
  assessConfig.maxAnnounceWords =
      configIntOr("guard.max_announce_words", 12);
  assessConfig.vetoScope = configOr("guard.veto_scope", "soft_only");
  assessConfig.maxToolRounds = configIntOr("guard.assess.max_tool_rounds", 3);
  assessConfig.lang = configOr("guard.announce_lang", "es");
  GuardAssessment assessment(
      {.camera = actions.get(), .vlm = vlm.get(), .llm = llm.get()},
      assessConfig);

  GuardService::Config guardConfig;
  guardConfig.enabled = ConfigService::getBool("guard.enabled");
  guardConfig.profile = configOr("guard.profile", "home");
  guardConfig.defaultMode = guard_policy::modeFromString(
      configOr("guard.default_mode", "home"));
  guardConfig.notifyLevel = configIntOr("guard.notify_level", 2);
  guardConfig.announceLevel = configIntOr("guard.announce_level", 3);
  guardConfig.alarmLevel = configIntOr("guard.alarm_level", 4);
  guardConfig.announceText = configOr(
      "guard.announce_text", "Hola, ¿necesitas algo?");
  guardConfig.announceLang = configOr("guard.announce_lang", "es");
  guardConfig.greetEnabled = configBoolOr("guard.greet_enabled", true);
  guardConfig.greetKnown = configBoolOr("guard.greet_known", false);
  guardConfig.greetText = configOr(
      "guard.greet_text", "Hola, ¿necesitas algo?");
  guardConfig.greetTexts = configVariantsOr("guard.greet_texts", "");
  if (guardConfig.greetTexts.empty() && !guardConfig.greetText.empty())
    guardConfig.greetTexts.push_back(guardConfig.greetText);
  guardConfig.greetKnownText = configOr(
      "guard.greet_known_text", "Hola {name}, ¿necesitas algo?");
  guardConfig.greetLang = configOr("guard.greet_lang", guardConfig.announceLang);
  guardConfig.greetListenSeconds = configIntOr("guard.greet_listen_seconds", 6);
  guardConfig.greetReplyEnabled = configBoolOr("guard.greet_reply_enabled", true);
  guardConfig.greetReplyText = configOr(
      "guard.greet_reply_text", "Perfecto, dime, ¿en qué te ayudo?");
  guardConfig.greetReplyTexts = configVariantsOr("guard.greet_reply_texts", "");
  guardConfig.greetReplyLang =
      configOr("guard.greet_reply_lang", guardConfig.greetLang);
  guardConfig.greetRepairText = configOr(
      "guard.greet_repair_text", "No te escuché bien. ¿Puedes repetirlo?");
  guardConfig.alarmSeconds = configIntOr("guard.alarm_seconds", 6);
  guardConfig.armSiren = configBoolOr("guard.arm_siren", false);
  guardConfig.sirenSeconds = configIntOr("guard.siren_seconds", 20);
  guardConfig.vetoScope = assessConfig.vetoScope;
  guardConfig.actionCooldownS = ConfigService::getInt("guard.action_cooldown_s");
  if (guardConfig.actionCooldownS <= 0)
    guardConfig.actionCooldownS = 120;
  guardConfig.repeatWindowS = ConfigService::getInt("guard.repeat_window_s");
  if (guardConfig.repeatWindowS <= 0)
    guardConfig.repeatWindowS = 86400;
  guardConfig.maxActionsPerHour =
      configIntOr("guard.max_actions_per_hour", 4);
  guardConfig.expectedGuestsEnabled =
      configBoolOr("guard.expected_guests", true);

  guardConfig.crossCameraWindowS =
      configIntOr("guard.cross_camera_window_s", 60);
  guardConfig.continuityWindowS =
      configIntOr("guard.continuity_window_s", 20);
  guardConfig.signatureMinSimilarity =
      ConfigService::getDouble("guard.signature_min_similarity");
  if (guardConfig.signatureMinSimilarity <= 0)
    guardConfig.signatureMinSimilarity = 0.82;
  guardConfig.loiterChecks = configIntOr("guard.loiter_checks", 3);
  guardConfig.stagingEnabled = configBoolOr("guard.staging", true);
  guardConfig.encounterTimeoutS =
      configIntOr("guard.encounter_timeout_s", 300);
  guardConfig.heartbeatS = configIntOr("guard.heartbeat_s", 10);
  guardConfig.maxDialogueTurns = configIntOr("guard.max_dialogue_turns", 3);
  guardConfig.maxObservationAttempts =
      configIntOr("guard.max_observation_attempts", 5);
  guardConfig.maxAnnounceWords = configIntOr("guard.max_announce_words", 12);
  guardConfig.consumerDurable =
      configOr("guard.consumer_durable", "argus-guard");
  guardConfig.eventStream =
      configOr("guard.event_stream", "ARGUS_CAMERA");
  guardConfig.eventSubject = ConfigService::getString("guard.event_subject");
  guardConfig.decisionMode = configOr("guard.decision_mode", "shadow");
  if (guardConfig.decisionMode != "shadow" &&
      guardConfig.decisionMode != "enforce") {
    LOG_WARN << "Unknown guard.decision_mode '"
             << guardConfig.decisionMode << "'; using shadow";
    guardConfig.decisionMode = "shadow";
  }
  const auto gateScope = beliefGateScopeFromString(
      configOr("guard.belief.gate_scope", "notify"));
  if (!gateScope) {
    LOG_WARN << "Unknown guard.belief.gate_scope '"
             << configOr("guard.belief.gate_scope", "notify")
             << "'; using notify";
    guardConfig.beliefGateScope = BeliefGateScope::Notify;
  }
  else {
    guardConfig.beliefGateScope = *gateScope;
  }
  guardConfig.healthStaleS = configIntOr("guard.health_stale_s", 300);
  if (guardConfig.healthStaleS <= 0)
    guardConfig.healthStaleS = 300;
  guardConfig.beliefRefreshS = configIntOr("guard.belief_refresh_s", 300);
  if (guardConfig.beliefRefreshS <= 0)
    guardConfig.beliefRefreshS = 300;
  guardConfig.journalRetentionDays =
      configIntOr("guard.journal_retention_days", 90);
  guardConfig.quietHoursEnabled =
      configBoolOr("guard.quiet_hours.enabled", false);
  guardConfig.quietStartHour = configIntOr("guard.quiet_hours.start_hour", 22);
  if (guardConfig.quietStartHour < 0 || guardConfig.quietStartHour > 23)
    guardConfig.quietStartHour = 22;
  guardConfig.quietEndHour = configIntOr("guard.quiet_hours.end_hour", 7);
  if (guardConfig.quietEndHour < 0 || guardConfig.quietEndHour > 23)
    guardConfig.quietEndHour = 7;
  guardConfig.quietDailyBudget =
      configIntOr("guard.quiet_hours.daily_budget", 30);
  if (guardConfig.quietDailyBudget <= 0)
    guardConfig.quietDailyBudget = 30;
  guardConfig.tamperSustainedS =
      configIntOr("guard.tamper_sustained_s", 300);
  if (guardConfig.tamperSustainedS <= 0)
    guardConfig.tamperSustainedS = 300;
  std::shared_ptr<NatsBus> natsBus;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; guard consumer disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; guard keeps retrying the durable consumer";
  }

  GuardService guardService(
      {.bus = natsBus.get(),
       .identity = identity.get(),
       .notifications = notifications.get(),
       .actions = actions.get(),
       .assessment = &assessment},
      guardConfig);

  registerHealth();
  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());
  drogon::app().registerController(
      std::make_shared<GuardController>(identity.get()));

  drogon::app().loadConfigJson(
      drogonConfig({.dbPath = dbPath, .host = host, .port = port}));

  drogon::app().registerBeginningAdvice([&schemaPath]() {
    if (!guard_schema::migrate(schemaPath)) {
      LOG_FATAL << "Guard database migration failed — aborting startup";
      _exit(1);
    }
    if (!DbService::runScriptFile(schemaPath)) {
      LOG_FATAL << "Guard database schema failed to apply — aborting startup";
      _exit(1);
    }
    DbService::applyPragmas();
  });

  LOG_INFO << "Listening on " << host << ":" << port << " (plain); guard database "
           << dbPath;

  drogon::app().registerBeginningAdvice([&guardService]() {
    guardService.start();
  });

  drogon::app().setThreadNum(0).run();
  return 0;
}
