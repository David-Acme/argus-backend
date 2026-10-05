#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/guard-config.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/settings/guard-settings.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace
{
namespace wire = argus::settings::v1;

constexpr const char* kSettingsSecret = "guard-settings-test-settings";
constexpr const char* kOtherSecret = "guard-settings-test-other";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() /
               ("guard-settings-test-" + std::to_string(::getpid()) + ".toml"))
                  .string())
  {
    std::ofstream(path_) << content;
    ConfigService::load(path_);
  }

  ~ScopedConfig()
  {
    std::ofstream(path_).flush();
    ConfigService::load(path_);
    std::remove(path_.c_str());
  }

  ScopedConfig(const ScopedConfig&) = delete;
  ScopedConfig& operator=(const ScopedConfig&) = delete;

private:
  std::string path_;
};

std::string fallbackOf(const std::vector<SettingSpec>& catalog,
                       std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE_MESSAGE(spec != catalog.end(), key);
  return spec->fallback;
}

int intFallback(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  return std::stoi(fallbackOf(catalog, key));
}

bool toggleFallback(const std::vector<SettingSpec>& catalog,
                    std::string_view key)
{
  return fallbackOf(catalog, key) == "true";
}

double decimalFallback(const std::vector<SettingSpec>& catalog,
                       std::string_view key)
{
  return std::stod(fallbackOf(catalog, key));
}

struct CallInput
{
  std::string target;
  std::string secret;
};

grpc::StatusCode listWith(const CallInput& input)
{
  auto stub = wire::Settings::NewStub(
      grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  if (!input.secret.empty())
    argus::client::addCallerCredential(context, input.secret);
  wire::SettingsCatalog catalog;
  return stub->List(&context, {}, &catalog).error_code();
}

struct UpdateInput
{
  std::string target;
  std::string secret;
  std::string key;
  std::string value;
};

grpc::StatusCode updateWith(const UpdateInput& input)
{
  auto stub = wire::Settings::NewStub(
      grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  argus::client::addCallerCredential(context, input.secret);
  wire::UpdateSettingsRequest request;
  auto* change = request.add_changes();
  change->set_key(input.key);
  change->set_value(input.value);
  wire::UpdateSettingsResponse response;
  return stub->Update(&context, request, &response).error_code();
}

}

TEST_CASE("the guard catalog builds through the registry")
{
  const auto catalog = guardSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{guardSettingsCatalog()});
  CHECK(catalog.size() == 42);
  CHECK(std::ranges::count(catalog, SettingLevel::Basic, &SettingSpec::level) ==
        12);
  for (const auto& spec : catalog) {
    CHECK_MESSAGE(spec.apply == SettingApply::Live, spec.key);
    CHECK_MESSAGE(spec.key.starts_with("guard."), spec.key);
  }
}

TEST_CASE("the guard catalog names no plumbing")
{
  constexpr std::array kForbidden{
      "path", "file", "target", "url", "credential", "secret", "port",
      "address", "model", ".db", "schema", "caller", "consumer", "durable",
      "stream", "subject", "nats", "jwt", "identity", "rpc", "assess",
      "text", "lang"};
  for (const auto& spec : guardSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
  CHECK(std::ranges::none_of(guardSettingsCatalog(), [](const SettingSpec& spec) {
    return spec.key == "guard.enabled" || spec.key == "guard.default_mode" ||
           spec.key == "guard.profile";
  }));
}

TEST_CASE("every fallback is what the guard runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = guardSettingsCatalog();
  const GuardServiceConfig service = GuardConfig::resolveService();
  CHECK(intFallback(catalog, "guard.notify_level") == service.notifyLevel);
  CHECK(intFallback(catalog, "guard.announce_level") == service.announceLevel);
  CHECK(intFallback(catalog, "guard.alarm_level") == service.alarmLevel);
  CHECK(intFallback(catalog, "guard.alarm_seconds") == service.alarmSeconds);
  CHECK(toggleFallback(catalog, "guard.arm_siren") == service.armSiren);
  CHECK(intFallback(catalog, "guard.siren_seconds") == service.sirenSeconds);
  CHECK(toggleFallback(catalog, "guard.greet_enabled") == service.greetEnabled);
  CHECK(toggleFallback(catalog, "guard.greet_known") == service.greetKnown);
  CHECK(toggleFallback(catalog, "guard.expected_guests") ==
        service.expectedGuestsEnabled);
  CHECK(toggleFallback(catalog, "guard.quiet_hours.enabled") ==
        service.quietHoursEnabled);
  CHECK(intFallback(catalog, "guard.quiet_hours.start_hour") ==
        service.quietStartHour);
  CHECK(intFallback(catalog, "guard.quiet_hours.end_hour") ==
        service.quietEndHour);
  CHECK(intFallback(catalog, "guard.quiet_hours.daily_budget") ==
        service.quietDailyBudget);
  CHECK(fallbackOf(catalog, "guard.decision_mode") == service.decisionMode);
  CHECK(fallbackOf(catalog, "guard.belief.gate_scope") ==
        beliefGateScopeToString(service.beliefGateScope));
  CHECK(intFallback(catalog, "guard.belief_refresh_s") == service.beliefRefreshS);
  CHECK(intFallback(catalog, "guard.action_cooldown_s") ==
        service.actionCooldownS);
  CHECK(intFallback(catalog, "guard.repeat_window_s") == service.repeatWindowS);
  CHECK(intFallback(catalog, "guard.regroup_window_s") == service.regroupWindowS);
  CHECK(intFallback(catalog, "guard.max_actions_per_hour") ==
        service.maxActionsPerHour);
  CHECK(intFallback(catalog, "guard.max_dialogue_turns") ==
        service.maxDialogueTurns);
  CHECK(intFallback(catalog, "guard.greet_listen_seconds") ==
        service.greetListenSeconds);
  CHECK(toggleFallback(catalog, "guard.greet_reply_enabled") ==
        service.greetReplyEnabled);
  CHECK(intFallback(catalog, "guard.cross_camera_window_s") ==
        service.crossCameraWindowS);
  CHECK(intFallback(catalog, "guard.continuity_window_s") ==
        service.continuityWindowS);
  CHECK(decimalFallback(catalog, "guard.signature_min_similarity") ==
        doctest::Approx(service.signatureMinSimilarity));
  CHECK(intFallback(catalog, "guard.loiter_checks") == service.loiterChecks);
  CHECK(toggleFallback(catalog, "guard.staging") == service.stagingEnabled);
  CHECK(intFallback(catalog, "guard.encounter_timeout_s") ==
        service.encounterTimeoutS);
  CHECK(intFallback(catalog, "guard.tamper_sustained_s") ==
        service.tamperSustainedS);
  CHECK(intFallback(catalog, "guard.health_stale_s") == service.healthStaleS);
  CHECK(intFallback(catalog, "guard.journal_retention_days") ==
        service.journalRetentionDays);
  CHECK(intFallback(catalog, "guard.marked_retention_days") ==
        service.markedRetentionDays);

  const BeliefConfig belief = GuardConfig::resolveBelief(1);
  CHECK(intFallback(catalog, "guard.belief.threshold_critical") ==
        belief.thresholdCritical);
  CHECK(intFallback(catalog, "guard.belief.threshold_high") ==
        belief.thresholdHigh);
  CHECK(intFallback(catalog, "guard.belief.threshold_medium") ==
        belief.thresholdMedium);
  CHECK(intFallback(catalog, "guard.belief.threshold_low") ==
        belief.thresholdLow);
  CHECK(decimalFallback(catalog, "guard.belief.detector_strong") ==
        doctest::Approx(belief.detectorStrong));
  CHECK(decimalFallback(catalog, "guard.belief.detector_weak") ==
        doctest::Approx(belief.detectorWeak));
  CHECK(intFallback(catalog, "guard.belief.zone_dwell_alert_ms") ==
        belief.zoneDwellAlertMs);
  CHECK(intFallback(catalog, "guard.belief.zone_dwell_monitor_ms") ==
        belief.zoneDwellMonitorMs);
}

TEST_CASE("a quiet window starting at midnight is honoured, not replaced")
{
  const ScopedConfig config(
      "[guard.quiet_hours]\nstart_hour = 0\nend_hour = 0\n");
  const GuardServiceConfig service = GuardConfig::resolveService();
  CHECK(service.quietStartHour == 0);
  CHECK(service.quietEndHour == 0);
}

TEST_CASE("a registry update reaches a running guard through the refresh")
{
  const ScopedConfig config(
      "[guard]\nnotify_level = 2\ndecision_mode = \"shadow\"\n"
      "consumer_durable = \"guard-settings-boot\"\n"
      "[guard.quiet_hours]\nenabled = false\nstart_hour = 22\n");
  GuardService::Config boot = GuardConfig::resolveService();
  boot.retryLeaseMs = 4321;
  GuardService service({.bus = nullptr,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       boot);
  service.start();

  SettingsRegistry registry(guardSettingsCatalog());
  std::vector<std::string> heard;
  registry.onChange([&service, &heard](const std::vector<std::string>& keys) {
    heard = keys;
    service.refresh(GuardConfig::resolveService());
  });

  const auto before = service.currentConfig();
  REQUIRE(ConfigService::setString("guard.consumer_durable", "guard-settings-new"));
  const auto result = registry.update(
      {{.key = "guard.notify_level", .value = "3"},
       {.key = "guard.decision_mode", .value = "enforce"},
       {.key = "guard.quiet_hours.enabled", .value = "true"},
       {.key = "guard.quiet_hours.start_hour", .value = "0"},
       {.key = "guard.belief.gate_scope", .value = "all"},
       {.key = "guard.signature_min_similarity", .value = "0.9"},
       {.key = "guard.regroup_window_s", .value = "0"}});
  CHECK(result.rejected.empty());
  CHECK(heard.size() == 7);

  const auto after = service.currentConfig();
  CHECK(after->notifyLevel == 3);
  CHECK(after->decisionMode == "enforce");
  CHECK(after->quietHoursEnabled);
  CHECK(after->quietStartHour == 0);
  CHECK(after->beliefGateScope == BeliefGateScope::All);
  CHECK(after->signatureMinSimilarity == doctest::Approx(0.9));
  CHECK(after->regroupWindowS == 0);
  CHECK(after->consumerDurable == "guard-settings-boot");
  CHECK(after->retryLeaseMs == 4321);

  CHECK(before->notifyLevel == 2);
  CHECK(before->decisionMode == "shadow");
  CHECK_FALSE(before->quietHoursEnabled);
  CHECK(before->regroupWindowS == 600);
}

TEST_CASE("a regroup window of zero turns grouping off instead of falling back")
{
  const auto windowOf = [](const std::string& content) {
    const ScopedConfig config(content);
    return GuardConfig::resolveService().regroupWindowS;
  };
  CHECK(windowOf("[guard]\nregroup_window_s = 0\n") == 0);
  CHECK(windowOf("[guard]\nregroup_window_s = 900\n") == 900);
  CHECK(windowOf("[guard]\nnotify_level = 2\n") == 600);
  CHECK(windowOf("[guard]\nregroup_window_s = -5\n") == 600);
}

TEST_CASE("the registry refuses what the guard could not use")
{
  const ScopedConfig config("[guard]\nnotify_level = 2\n");
  SettingsRegistry registry(guardSettingsCatalog());
  const auto result = registry.update(
      {{.key = "guard.notify_level", .value = "5"},
       {.key = "guard.decision_mode", .value = "loud"},
       {.key = "guard.alarm_seconds", .value = "0"},
       {.key = "guard.consumer_durable", .value = "x"},
       {.key = "guard.enabled", .value = "true"}});
  CHECK(result.applied.empty());
  REQUIRE(result.rejected.size() == 5);
  CHECK(result.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[1].reason == SettingRejectionReason::NotAChoice);
  CHECK(result.rejected[2].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[3].reason == SettingRejectionReason::Unknown);
  CHECK(result.rejected[4].reason == SettingRejectionReason::Unknown);
  CHECK(GuardConfig::resolveService().notifyLevel == 2);
}

TEST_CASE("only the settings caller reaches the guard settings listener")
{
  const ScopedConfig config(
      std::string("[guard]\nnotify_level = 2\n[rpc]\naddress = \"127.0.0.1:0\"\n"
                  "[rpc.callers]\nother = \"") +
      kOtherSecret + "\"\nsettings = \"" + kSettingsSecret + "\"\n");
  GuardRpcConfig rpc = GuardConfig::resolveRpc();
  CHECK(rpc.address == "127.0.0.1:0");
  REQUIRE(rpc.settingsCredentials.size() == 1);
  CHECK(rpc.settingsCredentials[0].service == kSettingsCaller);

  SettingsRegistry registry(guardSettingsCatalog());
  SettingsRpcService settings(
      SettingsRpcInput{.service = "guard",
                       .registry = &registry,
                       .credentials = std::move(rpc.settingsCredentials)});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(rpc.address, grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&settings);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(listWith({.target = target, .secret = kSettingsSecret}) ==
        grpc::StatusCode::OK);
  CHECK(listWith({.target = target, .secret = kOtherSecret}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(listWith({.target = target, .secret = "wrong"}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(listWith({.target = target, .secret = ""}) ==
        grpc::StatusCode::UNAUTHENTICATED);

  CHECK(updateWith({.target = target,
                    .secret = kOtherSecret,
                    .key = "guard.notify_level",
                    .value = "3"}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(GuardConfig::resolveService().notifyLevel == 2);
  CHECK(updateWith({.target = target,
                    .secret = kSettingsSecret,
                    .key = "guard.notify_level",
                    .value = "3"}) == grpc::StatusCode::OK);
  CHECK(GuardConfig::resolveService().notifyLevel == 3);

  server->Shutdown(std::chrono::system_clock::now());
}

TEST_CASE("an empty settings secret or address publishes no listener")
{
  const ScopedConfig config("[rpc]\naddress = \"\"\n[rpc.callers]\nsettings = \"\"\n");
  const GuardRpcConfig rpc = GuardConfig::resolveRpc();
  CHECK(rpc.address.empty());
  CHECK(rpc.settingsCredentials.empty());
}
