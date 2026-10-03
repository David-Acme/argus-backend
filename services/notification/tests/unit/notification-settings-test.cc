#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/notification-rpc-service.hxx>
#include <config/config-service.hxx>
#include <config/notification-config.hxx>
#include <feature/camera-notification/services/camera-notification-policy.hxx>
#include <feature/camera-notification/services/camera-object-notifier.hxx>
#include <feature/settings/notification-settings.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace wire = argus::settings::v1;

constexpr const char* kGuardSecret = "notification-settings-test-guard";
constexpr const char* kSyncSecret = "notification-settings-test-sync";
constexpr const char* kSettingsSecret = "notification-settings-test-settings";
constexpr std::string_view kSyncRefusal = "argus-sync caller credential required";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "notification-settings-test.toml").string())
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

std::string callersConfig(const std::string& settingsSecret)
{
  return std::string("[grpc]\ncaller_guard = \"") + kGuardSecret + "\"\ncaller_sync = \"" +
         kSyncSecret + "\"\ncaller_settings = \"" + settingsSecret + "\"\n";
}

std::string fallbackOf(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE(spec != catalog.end());
  return spec->fallback;
}

int64_t epochMsAtLocalHour(int hour)
{
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  local.tm_hour = hour;
  local.tm_min = 30;
  local.tm_sec = 0;
  return static_cast<int64_t>(std::mktime(&local)) * 1000;
}

struct CallerProbe
{
  std::string target;
  std::string secret;
};

grpc::StatusCode listWith(const CallerProbe& probe)
{
  const std::string& target = probe.target;
  const std::string& secret = probe.secret;
  auto stub = wire::Settings::NewStub(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, secret);
  wire::SettingsCatalog catalog;
  return stub->List(&context, {}, &catalog).error_code();
}

grpc::StatusCode createWith(const CallerProbe& probe)
{
  const std::string& target = probe.target;
  const std::string& secret = probe.secret;
  auto stub = argus::notification::v1::NotificationService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, secret);
  argus::notification::v1::CreateNotificationsResponse response;
  return stub->CreateNotifications(&context, {}, &response).error_code();
}

grpc::Status pullWith(const CallerProbe& probe)
{
  const std::string& target = probe.target;
  const std::string& secret = probe.secret;
  auto stub = argus::notification::v1::NotificationService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, secret);
  argus::notification::v1::PullNotificationsResponse response;
  return stub->PullNotifications(&context, {}, &response);
}

}

TEST_CASE("the notification catalog builds through the registry")
{
  const auto catalog = notificationSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{notificationSettingsCatalog()});
  CHECK(catalog.size() == 10);
  CHECK(std::ranges::count(catalog, SettingLevel::Basic, &SettingSpec::level) == 4);
  for (const auto& spec : catalog)
    CHECK_MESSAGE((spec.apply == SettingApply::Restart) ==
                      (spec.key == "notifications.selftest_interval_s"),
                  spec.key);
}

TEST_CASE("the notification catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "target", "url", "credential", "secret",
                                  "port", "address", "model", ".db", "schema", "caller",
                                  "identity", "nats", "push", "jwt"};
  for (const auto& spec : notificationSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every fallback is what the service runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = notificationSettingsCatalog();
  const CameraNotificationPolicy::Config policy = camera_notifier::resolveConfig();
  CHECK(std::stoi(fallbackOf(catalog, "notifications.budget_per_hour")) == policy.budgetPerHour);
  CHECK(std::stoi(fallbackOf(catalog, "notifications.silent_start")) == policy.silentStartHour);
  CHECK(std::stoi(fallbackOf(catalog, "notifications.silent_end")) == policy.silentEndHour);
  CHECK(std::stoll(fallbackOf(catalog, "notifications.guard_heartbeat_timeout_s")) * 1000 ==
        policy.guardTimeoutMs);
  CHECK(std::stod(fallbackOf(catalog, "notifications.fallback_min_score_median")) ==
        doctest::Approx(policy.fallbackMinScoreMedian));
  CHECK(std::stoll(fallbackOf(catalog, "notifications.fallback_min_dwell_ms")) ==
        policy.fallbackMinDwellMs);
  CHECK((fallbackOf(catalog, "notifications.fallback_suppress_known") == "true") ==
        policy.fallbackSuppressKnown);
  CHECK(std::stoi(fallbackOf(catalog, "notifications.fallback_retention_days")) ==
        policy.fallbackRetentionDays);
  CHECK(std::stoll(fallbackOf(catalog, "notifications.ack_window_s")) ==
        NotificationConfig::resolveAckWindowS());
  CHECK(std::stoll(fallbackOf(catalog, "notifications.selftest_interval_s")) ==
        NotificationConfig::resolveSelfTestIntervalS());
}

TEST_CASE("one quiet-hour bound alone keeps quiet hours off, as the fallback says")
{
  const ScopedConfig config("[notifications]\nsilent_start = 22\n");
  const CameraNotificationPolicy::Config policy = camera_notifier::resolveConfig();
  CHECK(policy.silentEndHour == -1);
  for (int hour = 0; hour < 24; ++hour)
    CHECK_FALSE(CameraNotificationPolicy::inSilentHours(policy, hour));
}

TEST_CASE("a quiet window and budget changed through the registry reach a running policy")
{
  const ScopedConfig config("[notifications]\nbudget_per_hour = 6\nsilent_start = -1\n"
                            "silent_end = -1\nack_window_s = 86400\n");
  CameraNotificationPolicy policy(camera_notifier::resolveConfig());
  SettingsRegistry registry(notificationSettingsCatalog());
  std::vector<std::string> notified;
  registry.onChange([&policy, &notified](const std::vector<std::string>& keys) {
    notified = keys;
    policy.reconfigure(camera_notifier::resolveConfig());
  });

  const int64_t noon = epochMsAtLocalHour(12);
  CHECK(policy.shouldNotify(1, noon));

  const auto result = registry.update({{.key = "notifications.silent_start", .value = "11"},
                                       {.key = "notifications.silent_end", .value = "13"},
                                       {.key = "notifications.budget_per_hour", .value = "1"},
                                       {.key = "notifications.ack_window_s", .value = "7200"}});
  CHECK(result.rejected.empty());
  CHECK(notified.size() == 4);
  CHECK(policy.config().silentStartHour == 11);
  CHECK(policy.config().silentEndHour == 13);
  CHECK(policy.config().budgetPerHour == 1);
  CHECK_FALSE(policy.shouldNotify(2, noon));
  CHECK(NotificationConfig::resolveAckWindowS() == 7200);

  CHECK(registry.update({{.key = "notifications.silent_start", .value = "-1"}}).rejected.empty());
  CHECK(policy.shouldNotify(2, noon));
  CHECK_FALSE(policy.shouldNotify(2, noon + 1000));
}

TEST_CASE("the registry refuses values the policy could not use")
{
  const ScopedConfig config("[notifications]\nbudget_per_hour = 6\n");
  SettingsRegistry registry(notificationSettingsCatalog());
  const auto result = registry.update({{.key = "notifications.budget_per_hour", .value = "0"},
                                       {.key = "notifications.silent_end", .value = "24"},
                                       {.key = "notifications.db", .value = "/tmp/x.db"}});
  CHECK(result.applied.empty());
  REQUIRE(result.rejected.size() == 3);
  CHECK(result.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[1].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[2].reason == SettingRejectionReason::Unknown);
  CHECK(camera_notifier::resolveConfig().budgetPerHour == 6);
}

TEST_CASE("the settings caller and the notification callers cannot stand in for each other")
{
  const ScopedConfig config(callersConfig(kSettingsSecret));
  const auto settingsCallers = NotificationConfig::resolveSettingsCallers();
  REQUIRE(settingsCallers.size() == 1);
  CHECK(settingsCallers[0].service == kSettingsCaller);

  SettingsRegistry registry(notificationSettingsCatalog());
  NotificationRpcService notifications(NotificationRpcService::Dependencies{
      .deliverySink = nullptr, .pushSink = nullptr, .pushRequired = false});
  SettingsRpcService settings(SettingsRpcInput{
      .service = "notification", .registry = &registry, .credentials = settingsCallers});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&notifications);
  builder.RegisterService(&settings);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(listWith({.target = target, .secret = kSettingsSecret}) == grpc::StatusCode::OK);
  CHECK(listWith({.target = target, .secret = kGuardSecret}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(listWith({.target = target, .secret = kSyncSecret}) == grpc::StatusCode::UNAUTHENTICATED);

  CHECK(createWith({.target = target, .secret = kSettingsSecret}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(createWith({.target = target, .secret = kGuardSecret}) == grpc::StatusCode::INVALID_ARGUMENT);

  const grpc::Status settingsPull = pullWith({.target = target, .secret = kSettingsSecret});
  CHECK(settingsPull.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(settingsPull.error_message() == kSyncRefusal);
  const grpc::Status syncPull = pullWith({.target = target, .secret = kSyncSecret});
  CHECK(syncPull.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(syncPull.error_message() != kSyncRefusal);

  server->Shutdown(std::chrono::system_clock::now());
}

TEST_CASE("a settings secret equal to another caller's publishes no settings caller")
{
  for (const char* reused : {kGuardSecret, kSyncSecret}) {
    const ScopedConfig config(callersConfig(reused));
    CHECK(NotificationConfig::resolveSettingsCallers().empty());
  }
}

TEST_CASE("an empty settings secret publishes no settings caller")
{
  const ScopedConfig config(callersConfig(""));
  CHECK(NotificationConfig::resolveSettingsCallers().empty());
}
