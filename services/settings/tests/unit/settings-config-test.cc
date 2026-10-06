#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-config.hxx>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>

namespace
{
std::filesystem::path configPath()
{
  return std::filesystem::temp_directory_path() / "argus-settings-config-test.toml";
}

void loadConfig(const std::string& content)
{
  std::ofstream(configPath()) << content;
  ConfigService::load(configPath().string());
}
}

TEST_CASE("owners resolve in the fixed display order and only when fully configured")
{
  loadConfig(R"([owners.notification]
target = "127.0.0.1:7038"
credential = "n"

[owners.tts]
target = "127.0.0.1:7129"
credential = "t"

[owners.llm]
target = "127.0.0.1:7132"
credential = "l"

[owners.voice]
target = "127.0.0.1:7034"
credential = ""

[owners.stt]
target = "127.0.0.1:7130"

[owners.vlm]
target = ""
credential = "v"

[owners.gateway]
target = "127.0.0.1:1"
credential = "g"
)");

  const auto owners = SettingsConfig::resolveOwners();
  REQUIRE(owners.size() == 3);
  CHECK(owners[0].name == "llm");
  CHECK(owners[0].target == "127.0.0.1:7132");
  CHECK(owners[0].credential == "l");
  CHECK(owners[1].name == "tts");
  CHECK(owners[2].name == "notification");
  CHECK(SettingsConfig::unconfiguredOwners(owners) ==
        std::vector<std::string>{"voice", "stt", "vlm", "guard", "camera"});
  std::filesystem::remove(configPath());
}

TEST_CASE("an owner may name the host path of its config file, and the first run is on unless turned off")
{
  loadConfig(R"([owners.tts]
target = "argus-tts:7129"
credential = "t"
config_file = "/srv/argus/argus-deploy/config.tts.toml"
)");
  const auto owners = SettingsConfig::resolveOwners();
  REQUIRE(owners.size() == 1);
  CHECK(owners[0].configFile == "/srv/argus/argus-deploy/config.tts.toml");
  const auto defaults = SettingsConfig::resolveFirstRun();
  CHECK(defaults.enabled);
  CHECK(defaults.interval == std::chrono::seconds(30));

  loadConfig("[settings]\nfirst_run = false\nfirst_run_interval_s = 120\n");
  const auto tuned = SettingsConfig::resolveFirstRun();
  CHECK_FALSE(tuned.enabled);
  CHECK(tuned.interval == std::chrono::seconds(120));

  loadConfig("[settings]\nfirst_run_interval_s = 1\n");
  CHECK(SettingsConfig::resolveFirstRun().interval == std::chrono::seconds(30));
  std::filesystem::remove(configPath());
}

TEST_CASE("the listener and the deadlines fall back to their defaults")
{
  loadConfig("[settings]\nlist_timeout_ms = 0\nupdate_timeout_ms = 999999\n");
  const auto listener = SettingsConfig::resolveListener();
  CHECK(listener.port == 7045);
  CHECK(listener.tls);
  const auto timeouts = SettingsConfig::resolveTimeouts();
  CHECK(timeouts.list.count() == 1500);
  CHECK(timeouts.update.count() == 5000);

  loadConfig("[settings]\nport = 7145\nplain = true\nlist_timeout_ms = 800\nupdate_timeout_ms = 3000\n");
  CHECK(SettingsConfig::resolveListener().port == 7145);
  CHECK_FALSE(SettingsConfig::resolveListener().tls);
  CHECK(SettingsConfig::resolveTimeouts().list.count() == 800);
  CHECK(SettingsConfig::resolveTimeouts().update.count() == 3000);
  std::filesystem::remove(configPath());
}

TEST_CASE("the profile file defaults beside the working directory and follows the config")
{
  loadConfig("[settings]\nport = 7045\n");
  CHECK(SettingsConfig::resolveProfilesPath() == "profiles.json");

  loadConfig("[settings]\nprofiles_path = \"/opt/argus/profiles.json\"\n");
  CHECK(SettingsConfig::resolveProfilesPath() == "/opt/argus/profiles.json");
  std::filesystem::remove(configPath());
}

TEST_CASE("the data-only owners resolve apart from the settings owners, so the settings screen never lists them")
{
  loadConfig(R"([owners.camera]
target = "127.0.0.1:7036"
credential = "c"

[owners.sync]
target = "127.0.0.1:7041"
credential = "s"

[owners.identity]
target = "127.0.0.1:7040"
credential = "i"

[owners.productivity]
target = "127.0.0.1:7037"
credential = ""
)");

  const auto owners = SettingsConfig::resolveOwners();
  REQUIRE(owners.size() == 1);
  CHECK(owners[0].name == "camera");
  const auto data = SettingsConfig::resolveDataOwners();
  REQUIRE(data.size() == 2);
  CHECK(data[0].name == "identity");
  CHECK(data[0].target == "127.0.0.1:7040");
  CHECK(data[1].name == "sync");
  CHECK(data[1].credential == "s");
  std::filesystem::remove(configPath());
}
