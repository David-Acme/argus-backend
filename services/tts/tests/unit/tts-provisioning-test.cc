#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>
#include <feature/provisioning/infra/pocket-catalog.hxx>
#include <feature/provisioning/infra/provision-process.hxx>
#include <feature/provisioning/services/pocket-choice-states.hxx>
#include <feature/provisioning/services/pocket-installer.hxx>
#include <feature/settings/tts-settings.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace
{
using namespace std::chrono_literals;

constexpr std::string_view kCatalog = R"(variant es-fast 219275806
variant es-quality 672425286
variant en 219274216
voice es-fast jean 6096696 cc-by-nc-4.0
voice es-fast lola 5457720 cc0
voice es-quality jean 24780120 cc-by-nc-4.0
voice es-quality lola 23797080 cc0
voice es-quality michael 29105504 cc-by-4.0
voice en jean 6195000 cc-by-nc-4.0
voice en alba 6195000 cc-by-4.0
voice en george 6244152 cc-by-4.0
)";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "tts-provisioning-test.toml").string())
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

struct Fixture
{
  std::set<std::string> installed;
  std::set<std::string> running;
  std::set<std::string> failed;

  [[nodiscard]] PocketProvisioningView view(const ProvisioningHost& host, bool nonCommercial) const
  {
    return {.catalog = PocketCatalog::parse(kCatalog),
            .host = host,
            .nonCommercialAllowed = nonCommercial,
            .installed = [this](const PocketComponent& component) { return installed.contains(component.id()); },
            .job = [this](const PocketComponent& component) -> std::optional<PocketInstallJob> {
              if (running.contains(component.id()))
                return PocketInstallJob::Running;
              if (failed.contains(component.id()))
                return PocketInstallJob::Failed;
              return std::nullopt;
            }};
  }
};

ProvisioningHost nativeHost()
{
  return {.script = std::filesystem::path("/repo/services/tts/scripts/provision.sh"), .canDownload = true, .canExport = true};
}

ProvisioningHost containerHost()
{
  return {.script = std::nullopt, .canDownload = false, .canExport = false};
}

const SettingSpec& specOf(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE(spec != catalog.end());
  return *spec;
}

const ChoiceState& stateOf(const std::vector<ChoiceState>& states, std::string_view choice)
{
  const auto state = std::ranges::find(states, choice, &ChoiceState::choice);
  REQUIRE(state != states.end());
  return *state;
}

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class FakeRepository
{
public:
  explicit FakeRepository(const std::string& body)
      : root_(std::filesystem::temp_directory_path() / ("tts-provisioning-repo-" + std::to_string(::getpid())))
  {
    std::filesystem::create_directories(root_ / "models" / "tts" / "pocket");
    std::filesystem::create_directories(root_ / "services" / "tts" / "scripts");
    std::ofstream(script()) << "#!/usr/bin/env bash\n" << body;
  }

  ~FakeRepository() { std::filesystem::remove_all(root_); }

  FakeRepository(const FakeRepository&) = delete;
  FakeRepository& operator=(const FakeRepository&) = delete;

  [[nodiscard]] std::filesystem::path root() const { return root_; }
  [[nodiscard]] std::filesystem::path script() const { return root_ / "services" / "tts" / "scripts" / "provision.sh"; }
  [[nodiscard]] ProvisioningPaths paths() const
  {
    return {.modelsDir = root_ / "models" / "tts", .pocketDir = root_ / "models" / "tts" / "pocket", .toolchainPython = {}};
  }

private:
  std::filesystem::path root_;
};

bool settle(const PocketInstaller& installer, const PocketComponent& component, std::optional<PocketInstallJob> wanted)
{
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (installer.job(component) == wanted)
      return true;
    std::this_thread::sleep_for(25ms);
  }
  return false;
}
}

TEST_CASE("the catalog written by provisioning is read with its sizes and licences, and nothing else")
{
  const auto catalog = PocketCatalog::parse(std::string(kCatalog) +
                                            "variant ../x 12\nvoice en bob 0 cc0\nvoice en Eve 12 cc0\nnonsense\n");
  CHECK(catalog.variants.size() == 3);
  CHECK(catalog.voices.size() == 8);
  REQUIRE(catalog.variant("es-quality") != nullptr);
  CHECK(catalog.variant("es-quality")->bytes == 672425286);
  REQUIRE(catalog.voice("en", "jean") != nullptr);
  CHECK(catalog.voice("en", "jean")->nonCommercial());
  CHECK_FALSE(catalog.voice("en", "alba")->nonCommercial());
  CHECK(catalog.voice("en", "lola") == nullptr);
  CHECK(PocketCatalog::load("/nonexistent/catalog.txt").variants.empty());
  CHECK(isComponentToken("es-quality"));
  CHECK_FALSE(isComponentToken("../es"));
  CHECK_FALSE(isComponentToken(""));
}

TEST_CASE("a component names the exact provisioning step and the host command that runs it")
{
  const PocketComponent variant{.kind = PocketComponentKind::Variant, .variant = "es-quality", .voice = {}};
  const PocketComponent voice{.kind = PocketComponentKind::Voice, .variant = "en", .voice = "jean"};
  CHECK(variant.arguments() == std::vector<std::string>{"--variant", "es-quality"});
  CHECK(voice.arguments() == std::vector<std::string>{"--voice", "en:jean"});
  CHECK(pocketHostCommand(variant, false) == "services/tts/scripts/provision.sh --variant es-quality");
  CHECK(pocketHostCommand(voice, true) ==
        "ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1 services/tts/scripts/provision.sh --voice en:jean");
}

TEST_CASE("in a container every missing component is a host step, and installed ones say so")
{
  const ScopedConfig config("");
  const auto catalog = ttsSettingsCatalog();
  Fixture fixture{.installed = {"variant es-fast", "voice es-quality:lola"}, .running = {}, .failed = {}};
  const auto view = fixture.view(containerHost(), false);

  const auto variants = pocketChoiceStates(specOf(catalog, "tts.pocket_variant_es"), view);
  CHECK(stateOf(variants, "fast").availability == ChoiceAvailability::Installed);
  CHECK(stateOf(variants, "fast").hostCommand.empty());
  const auto& quality = stateOf(variants, "quality");
  CHECK(quality.availability == ChoiceAvailability::HostOnly);
  CHECK(quality.sizeMb == doctest::Approx(672.425286));
  CHECK(quality.hostCommand == "services/tts/scripts/provision.sh --variant es-quality");

  const auto voices = pocketChoiceStates(specOf(catalog, "tts.pocket_voice_es"), view);
  REQUIRE(voices.size() == 9);
  CHECK(stateOf(voices, "lola").availability == ChoiceAvailability::Installed);
  CHECK(stateOf(voices, "jean").availability == ChoiceAvailability::HostOnly);
  CHECK(stateOf(voices, "jean").hostCommand.starts_with("ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1 "));
  CHECK(stateOf(voices, "giovanni").sizeMb == 0);

  const auto engines = pocketChoiceStates(specOf(catalog, "tts.engine_es"), view);
  CHECK(stateOf(engines, "pocket").availability == ChoiceAvailability::Installed);
  CHECK(stateOf(engines, "supertonic").availability == ChoiceAvailability::Installed);
  const auto english = pocketChoiceStates(specOf(catalog, "tts.engine_en"), view);
  CHECK(stateOf(english, "pocket").availability == ChoiceAvailability::HostOnly);
  CHECK(pocketChoiceStates(specOf(catalog, "tts.speed"), view).empty());
  CHECK(pocketChoiceStates(specOf(catalog, "tts.quality"), view).empty());
}

TEST_CASE("natively a missing component can be installed, unless it is non-commercial and not opted in")
{
  const ScopedConfig config("");
  const auto catalog = ttsSettingsCatalog();
  Fixture fixture{.installed = {}, .running = {"voice en:george"}, .failed = {"variant en"}};

  const auto closed = fixture.view(nativeHost(), false);
  CHECK(stateOf(pocketChoiceStates(specOf(catalog, "tts.pocket_variant_es"), closed), "quality").availability ==
        ChoiceAvailability::Installable);
  const auto english = pocketChoiceStates(specOf(catalog, "tts.pocket_voice_en"), closed);
  CHECK(stateOf(english, "jean").availability == ChoiceAvailability::HostOnly);
  CHECK(stateOf(english, "alba").availability == ChoiceAvailability::Installable);
  CHECK(stateOf(english, "alba").sizeMb == doctest::Approx(6.195));
  CHECK(stateOf(english, "george").availability == ChoiceAvailability::Installing);
  CHECK(stateOf(pocketChoiceStates(specOf(catalog, "tts.engine_en"), closed), "pocket").availability ==
        ChoiceAvailability::Failed);

  const auto open = fixture.view(nativeHost(), true);
  CHECK(stateOf(pocketChoiceStates(specOf(catalog, "tts.pocket_voice_en"), open), "jean").availability ==
        ChoiceAvailability::Installable);

  const ProvisioningHost voicesOnly{.script = nativeHost().script, .canDownload = true, .canExport = false};
  const auto partial = fixture.view(voicesOnly, true);
  CHECK(stateOf(pocketChoiceStates(specOf(catalog, "tts.pocket_variant_es"), partial), "quality").availability ==
        ChoiceAvailability::HostOnly);
  CHECK(stateOf(pocketChoiceStates(specOf(catalog, "tts.pocket_voice_es"), partial), "jean").availability ==
        ChoiceAvailability::Installable);
}

TEST_CASE("a Spanish voice is looked up in the variant the owner chose")
{
  const ScopedConfig config("[tts]\npocket_variant_es = \"fast\"\n");
  const auto catalog = ttsSettingsCatalog();
  Fixture fixture{.installed = {"voice es-quality:lola"}, .running = {}, .failed = {}};
  const auto states = pocketChoiceStates(specOf(catalog, "tts.pocket_voice_es"), fixture.view(nativeHost(), true));
  CHECK(stateOf(states, "lola").availability == ChoiceAvailability::Installable);
  CHECK(stateOf(states, "lola").sizeMb == doctest::Approx(5.45772));
  CHECK(stateOf(states, "lola").hostCommand == "services/tts/scripts/provision.sh --voice es-fast:lola");
}

TEST_CASE("only what the changed keys now need is installed, once")
{
  const ScopedConfig config("[tts]\nengine_en = \"supertonic\"\n");
  Fixture fixture{.installed = {"voice es-quality:lola"}, .running = {}, .failed = {}};
  const auto keys = std::vector<std::string>{"tts.pocket_variant_es", "tts.engine_en", "tts.speed"};

  const auto opted = pocketComponentsToInstall(keys, fixture.view(nativeHost(), true));
  REQUIRE(opted.size() == 2);
  CHECK(opted[0].id() == "variant es-quality");
  CHECK(opted[1].id() == "voice es-quality:jean");

  const auto closed = pocketComponentsToInstall(keys, fixture.view(nativeHost(), false));
  REQUIRE(closed.size() == 1);
  CHECK(closed[0].id() == "variant es-quality");

  fixture.running = {"variant es-quality"};
  CHECK(pocketComponentsToInstall(keys, fixture.view(nativeHost(), false)).empty());
  CHECK(pocketComponentsToInstall(keys, fixture.view(containerHost(), true)).empty());
  CHECK(pocketComponentsToInstall({"tts.speed"}, fixture.view(nativeHost(), true)).empty());

  const ScopedConfig supertonic("[tts]\nengine_es = \"supertonic\"\nengine_en = \"supertonic\"\n");
  CHECK(pocketComponentsToInstall(keys, fixture.view(nativeHost(), true)).empty());
}

TEST_CASE("the provisioning script is found only next to the models it would write")
{
  const FakeRepository repository("exit 0\n");
  CHECK(provisionScriptFor(repository.paths()).value_or(std::filesystem::path()) ==
        std::filesystem::canonical(repository.script()));

  auto elsewhere = repository.paths();
  elsewhere.pocketDir = std::filesystem::temp_directory_path();
  CHECK_FALSE(provisionScriptFor(elsewhere).has_value());

  const auto container = std::filesystem::temp_directory_path() / "tts-provisioning-container" / "models" / "tts";
  std::filesystem::create_directories(container / "pocket");
  CHECK_FALSE(
      provisionScriptFor({.modelsDir = container, .pocketDir = container / "pocket", .toolchainPython = {}}).has_value());
  CHECK_FALSE(probeProvisioningHost({.modelsDir = container, .pocketDir = container / "pocket", .toolchainPython = {}})
                  .canDownload);
  std::filesystem::remove_all(container.parent_path().parent_path());

  const auto host = probeProvisioningHost(repository.paths());
  CHECK(host.script.has_value());
  CHECK(host.canDownload == (onPath("bash") && onPath("curl")));
  CHECK_FALSE(onPath("argus-no-such-program"));
}

TEST_CASE("the installer runs one pinned step per component, with only the environment it chose")
{
  const FakeRepository repository(
      "printf '%s|%s|%s|%s\\n' \"$*\" \"${ARGUS_TTS_CONFIG:-}\" \"${ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES:-0}\" "
      "\"${ARGUS_TTS_POCKET_VARIANTS:-unset}\" >> \"$(dirname \"$0\")/calls.log\"\n"
      "echo \"installing $2\"\n"
      "[ \"$2\" = en:george ] && exit 3\n"
      "exit 0\n");
  ::setenv("ARGUS_TTS_POCKET_VARIANTS", "es-fast", 1);
  bool allowed = false;
  PocketInstaller installer({.script = repository.script(),
                             .configFile = repository.root() / "config.toml",
                             .nonCommercialAllowed = [&allowed] { return allowed; }});
  const PocketComponent variant{.kind = PocketComponentKind::Variant, .variant = "es-quality", .voice = {}};
  const PocketComponent george{.kind = PocketComponentKind::Voice, .variant = "en", .voice = "george"};
  const PocketComponent hostile{.kind = PocketComponentKind::Voice, .variant = "en", .voice = "../../x"};

  installer.request({variant, george, hostile});
  CHECK(settle(installer, variant, std::nullopt));
  CHECK(settle(installer, george, PocketInstallJob::Failed));
  CHECK_FALSE(installer.job(hostile).has_value());

  allowed = true;
  const PocketComponent jean{.kind = PocketComponentKind::Voice, .variant = "en", .voice = "jean"};
  installer.request({jean});
  CHECK(settle(installer, jean, std::nullopt));
  installer.stop();
  ::unsetenv("ARGUS_TTS_POCKET_VARIANTS");

  const auto calls = readFile(repository.script().parent_path() / "calls.log");
  const auto config = (repository.root() / "config.toml").string();
  CHECK(calls == "--variant es-quality|" + config + "|0|unset\n" + "--voice en:george|" + config + "|0|unset\n" +
                     "--voice en:jean|" + config + "|1|unset\n");
}

TEST_CASE("stopping the installer ends a running step and refuses new ones")
{
  const FakeRepository repository("touch \"$(dirname \"$0\")/started\"\nsleep 30\n");
  PocketInstaller installer({.script = repository.script(),
                             .configFile = repository.root() / "config.toml",
                             .nonCommercialAllowed = {}});
  const PocketComponent variant{.kind = PocketComponentKind::Variant, .variant = "en", .voice = {}};
  installer.request({variant});
  for (int attempt = 0; attempt < 200 && !std::filesystem::exists(repository.script().parent_path() / "started");
       ++attempt)
    std::this_thread::sleep_for(25ms);
  CHECK(installer.job(variant) == PocketInstallJob::Running);
  const auto start = std::chrono::steady_clock::now();
  installer.stop();
  CHECK(std::chrono::steady_clock::now() - start < 5s);
  std::filesystem::remove(repository.script().parent_path() / "started");
  installer.request({variant});
  std::this_thread::sleep_for(200ms);
  CHECK_FALSE(std::filesystem::exists(repository.script().parent_path() / "started"));
}
