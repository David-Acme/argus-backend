#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <feature/settings/dtos/response-apply-profile-dto.hxx>
#include <feature/settings/dtos/response-list-profiles-dto.hxx>
#include <feature/settings/infra/hardware-facts.hxx>
#include <feature/settings/infra/profile-file.hxx>
#include <feature/settings/services/first-run-service.hxx>
#include <feature/settings/services/profile-planner.hxx>
#include <feature/settings/services/settings-profile-service.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>
#include <json/reader.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;

constexpr const char* kSecret = "profile-secret";

SettingSpec choiceSpec(const std::string& key, std::vector<std::string> choices)
{
  return {.key = key,
          .group = "engine",
          .type = SettingType::Choice,
          .level = SettingLevel::Basic,
          .apply = SettingApply::Live,
          .range = {},
          .choices = std::move(choices),
          .fallback = "quality"};
}

SettingSpec integerSpec(const std::string& key)
{
  return {.key = key,
          .group = "descriptions",
          .type = SettingType::Integer,
          .level = SettingLevel::Basic,
          .apply = SettingApply::Live,
          .range = {.min = 128, .max = 1024, .step = 32},
          .choices = {},
          .fallback = "384"};
}

std::vector<SettingSpec> ttsSpecs()
{
  return {choiceSpec("tts.pocket_variant_es", {"fast", "quality"}),
          {.key = "tts.pocket_voice_es",
           .group = "engine",
           .type = SettingType::Choice,
           .level = SettingLevel::Advanced,
           .apply = SettingApply::Live,
           .range = {},
           .choices = {"jean", "lola"},
           .fallback = "lola"}};
}

std::vector<SettingSpec> vlmSpecs()
{
  return {integerSpec("vision.max_input_px")};
}

SettingEntry entry(const SettingSpec& spec, const std::string& value)
{
  return {.spec = spec, .value = value, .choiceStates = {}};
}

OwnerCatalog ttsCatalog(const std::string& variant, const std::string& voice)
{
  const auto specs = ttsSpecs();
  auto voiceEntry = entry(specs[1], voice);
  voiceEntry.choiceStates = {{.choice = "jean", .availability = ChoiceAvailability::HostOnly, .sizeMb = 30, .hostCommand = "provision"},
                             {.choice = "lola", .availability = ChoiceAvailability::Installed, .sizeMb = 25, .hostCommand = {}}};
  auto variantEntry = entry(specs[0], variant);
  variantEntry.choiceStates = {{.choice = "fast", .availability = ChoiceAvailability::Installable, .sizeMb = 219, .hostCommand = {}},
                               {.choice = "quality", .availability = ChoiceAvailability::Installed, .sizeMb = 672, .hostCommand = {}}};
  return {.service = "tts", .reachable = true, .settings = {variantEntry, voiceEntry}};
}

SettingsProfile qualityProfile()
{
  return {.id = "quality",
          .labelKey = "quality",
          .owners = {{.owner = "tts",
                      .changes = {{.key = "tts.pocket_voice_es", .value = "jean"},
                                  {.key = "tts.pocket_variant_es", .value = "quality"}}},
                     {.owner = "vlm", .changes = {{.key = "vision.max_input_px", .value = "512"}}}}};
}

ProfileCatalog catalogOf(std::vector<SettingsProfile> profiles)
{
  return {.profiles = std::move(profiles),
          .rules = {{.profile = "quality", .minCores = 8, .minRamGb = 14, .vectorIsa = true},
                    {.profile = "balanced", .minCores = 4, .minRamGb = 7, .vectorIsa = true}},
          .fallback = "performance"};
}

HardwareFacts hardware(int cores, double ramGb, CpuIsa isa)
{
  return {.cores = cores, .threads = cores * 2, .ramGb = ramGb, .isa = isa, .gpu = VideoAccel::None};
}

Json::Value parsed(const std::string& text)
{
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(text);
  REQUIRE(Json::parseFromStream(builder, stream, &root, &errors));
  return root;
}

std::filesystem::path configPath()
{
  return std::filesystem::temp_directory_path() / "argus-settings-profile-test.toml";
}

void loadConfig()
{
  std::ofstream(configPath()) << "[tts]\npocket_variant_es = \"quality\"\npocket_voice_es = \"lola\"\n\n[vision]\nmax_input_px = 384\n";
  ConfigService::load(configPath().string());
}

struct Owner
{
  SettingsRegistry registry;
  SettingsRpcService service;
  std::unique_ptr<grpc::Server> server;
  int port{0};

  Owner(const std::string& name, std::vector<SettingSpec> specs)
      : registry(std::move(specs)),
        service({.service = name, .registry = &registry, .credentials = {{.service = "settings", .secret = kSecret}}})
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~Owner() { server->Shutdown(); }

  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;

  [[nodiscard]] std::string target() const { return "127.0.0.1:" + std::to_string(port); }
};

int statusOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  return 0;
}

const ProfileKeyResult& resultOf(const OwnerApplyResult& owner, const std::string& key)
{
  const auto match = std::ranges::find(owner.results, key, &ProfileKeyResult::key);
  REQUIRE(match != owner.results.end());
  return *match;
}
}

TEST_CASE("values compare by their type, not by their spelling")
{
  const auto integer = entry(integerSpec("vision.max_input_px"), "384");
  CHECK(settings_profile::sameValue(integer, "384"));
  CHECK_FALSE(settings_profile::sameValue(integer, "+384"));
  CHECK_FALSE(settings_profile::sameValue(integer, "512"));
  SettingSpec decimal{.key = "tts.speed",
                      .group = "voice",
                      .type = SettingType::Decimal,
                      .level = SettingLevel::Basic,
                      .apply = SettingApply::Live,
                      .range = {.min = 0.5, .max = 2, .step = 0.05},
                      .choices = {},
                      .fallback = "1"};
  CHECK(settings_profile::sameValue(entry(decimal, "1.25"), "1.250"));
  CHECK_FALSE(settings_profile::sameValue(entry(decimal, "1.25"), "1.2"));
  CHECK(settings_profile::sameValue(entry(choiceSpec("tts.pocket_variant_es", {"fast", "quality"}), "fast"), "fast"));
}

TEST_CASE("a preview lists every key in catalog order with its current value, apply mode and install cost")
{
  const auto preview = settings_profile::preview(qualityProfile(), {ttsCatalog("fast", "lola")});
  CHECK(preview.id == "quality");
  CHECK_FALSE(preview.current);
  REQUIRE(preview.owners.size() == 2);

  const auto& tts = preview.owners[0];
  CHECK(tts.service == "tts");
  CHECK(tts.reachable);
  REQUIRE(tts.changes.size() == 2);
  CHECK(tts.changes[0].key == "tts.pocket_variant_es");
  CHECK(tts.changes[0].from == "fast");
  CHECK(tts.changes[0].to == "quality");
  CHECK(tts.changes[0].changed);
  CHECK(tts.changes[0].apply == SettingApply::Live);
  CHECK_FALSE(tts.changes[0].install.has_value());
  CHECK(tts.changes[1].key == "tts.pocket_voice_es");
  const auto install = tts.changes[1].install.value_or(ChoiceState{});
  CHECK(install.availability == ChoiceAvailability::HostOnly);
  CHECK(install.sizeMb == doctest::Approx(30));

  const auto& vlm = preview.owners[1];
  CHECK(vlm.service == "vlm");
  CHECK_FALSE(vlm.reachable);
  REQUIRE(vlm.changes.size() == 1);
  CHECK_FALSE(vlm.changes[0].from.has_value());
  CHECK(vlm.changes[0].to == "512");
}

TEST_CASE("a profile is current only when every owner answers and every key already holds its value")
{
  SettingsProfile tts{.id = "fast", .labelKey = "fast", .owners = {{.owner = "tts", .changes = {{.key = "tts.pocket_variant_es", .value = "fast"}}}}};
  CHECK(settings_profile::preview(tts, {ttsCatalog("fast", "lola")}).current);
  CHECK_FALSE(settings_profile::preview(tts, {ttsCatalog("quality", "lola")}).current);
  CHECK_FALSE(settings_profile::preview(tts, {}).current);

  tts.owners[0].changes.push_back({.key = "tts.unknown", .value = "x"});
  const auto preview = settings_profile::preview(tts, {ttsCatalog("fast", "lola")});
  CHECK_FALSE(preview.current);
  CHECK(preview.owners[0].changes.size() == 1);
}

TEST_CASE("the plan sends only what changes and settles the rest before any write")
{
  auto profile = qualityProfile();
  profile.owners[0].changes.push_back({.key = "tts.unknown", .value = "x"});
  const auto plans = settings_profile::plan(profile, {ttsCatalog("quality", "lola")});
  REQUIRE(plans.size() == 2);

  const ProfileApplication application(plans);
  const auto writes = application.pendingWrites();
  CHECK(writes.empty());

  const auto results = application.results();
  CHECK(resultOf(results[0], "tts.pocket_variant_es").status == ProfileKeyStatus::Unchanged);
  CHECK(resultOf(results[0], "tts.pocket_voice_es").status == ProfileKeyStatus::Rejected);
  CHECK(resultOf(results[0], "tts.pocket_voice_es").reason == SettingRejectionReason::NotInstalled);
  CHECK(resultOf(results[0], "tts.unknown").reason == SettingRejectionReason::Unknown);
  CHECK_FALSE(results[1].reachable);
  CHECK(resultOf(results[1], "vision.max_input_px").status == ProfileKeyStatus::Unreachable);
  CHECK_FALSE(results[1].catalog.has_value());
}

TEST_CASE("a refused key does not block the others: the owner is asked again without it")
{
  SettingsProfile profile{.id = "fast",
                          .labelKey = "fast",
                          .owners = {{.owner = "tts",
                                      .changes = {{.key = "tts.pocket_variant_es", .value = "fast"},
                                                  {.key = "tts.pocket_voice_es", .value = "jean"}}}}};
  auto catalog = ttsCatalog("quality", "lola");
  catalog.settings[1].choiceStates.clear();
  ProfileApplication application(settings_profile::plan(profile, {catalog}));

  auto writes = application.pendingWrites();
  REQUIRE(writes.size() == 1);
  CHECK(writes[0].changes.size() == 2);
  application.record({{.owner = "tts",
                       .reachable = true,
                       .applied = {},
                       .rejected = {{.key = "tts.pocket_voice_es", .reason = SettingRejectionReason::NotInstalled}},
                       .catalog = catalog}});

  writes = application.pendingWrites();
  REQUIRE(writes.size() == 1);
  REQUIRE(writes[0].changes.size() == 1);
  CHECK(writes[0].changes[0].key == "tts.pocket_variant_es");
  application.record({{.owner = "tts", .reachable = true, .applied = {"tts.pocket_variant_es"}, .rejected = {}, .catalog = catalog}});

  CHECK(application.pendingWrites().empty());
  const auto results = application.results();
  CHECK(resultOf(results[0], "tts.pocket_variant_es").status == ProfileKeyStatus::Applied);
  CHECK(resultOf(results[0], "tts.pocket_voice_es").reason == SettingRejectionReason::NotInstalled);
}

TEST_CASE("an owner that misses the write is unreachable, and a reply without progress ends the rounds")
{
  SettingsProfile profile{.id = "fast",
                          .labelKey = "fast",
                          .owners = {{.owner = "tts",
                                      .changes = {{.key = "tts.pocket_variant_es", .value = "fast"},
                                                  {.key = "tts.pocket_voice_es", .value = "jean"}}}}};
  auto catalog = ttsCatalog("quality", "lola");
  catalog.settings[1].choiceStates.clear();

  ProfileApplication missed(settings_profile::plan(profile, {catalog}));
  missed.record({{.owner = "tts", .reachable = false, .applied = {}, .rejected = {}, .catalog = std::nullopt}});
  CHECK(missed.pendingWrites().empty());
  const auto unreachable = missed.results();
  CHECK_FALSE(unreachable[0].reachable);
  CHECK(resultOf(unreachable[0], "tts.pocket_variant_es").status == ProfileKeyStatus::Unreachable);

  ProfileApplication stuck(settings_profile::plan(profile, {catalog}));
  stuck.record({{.owner = "tts",
                 .reachable = true,
                 .applied = {},
                 .rejected = {{.key = "tts.other", .reason = SettingRejectionReason::Invalid}},
                 .catalog = catalog}});
  CHECK(stuck.pendingWrites().empty());
  CHECK(resultOf(stuck.results()[0], "tts.pocket_variant_es").status == ProfileKeyStatus::Rejected);
}

TEST_CASE("the recommendation picks the first rule the host meets and names what held back the one above")
{
  const auto catalog = catalogOf({});

  const auto top = settings_profile::recommend(catalog, hardware(8, 30.7, CpuIsa::Avx2));
  CHECK(top.profile == "quality");
  CHECK(top.reason == RecommendationReason::Meets);
  REQUIRE(top.rule.has_value());
  CHECK_FALSE(top.missed.has_value());

  const auto fewCores = settings_profile::recommend(catalog, hardware(6, 30, CpuIsa::Avx512));
  CHECK(fewCores.profile == "balanced");
  CHECK(fewCores.reason == RecommendationReason::Cores);
  CHECK(fewCores.missed.value_or(RecommendationRule{}).profile == "quality");

  const auto lowRam = settings_profile::recommend(catalog, hardware(8, 12, CpuIsa::Avx2));
  CHECK(lowRam.profile == "balanced");
  CHECK(lowRam.reason == RecommendationReason::Ram);

  const auto small = settings_profile::recommend(catalog, hardware(2, 4, CpuIsa::Neon));
  CHECK(small.profile == "performance");
  CHECK(small.reason == RecommendationReason::Cores);
  CHECK_FALSE(small.rule.has_value());
  CHECK(small.missed.value_or(RecommendationRule{}).profile == "balanced");

  const auto scalar = settings_profile::recommend(catalog, hardware(16, 64, CpuIsa::Baseline));
  CHECK(scalar.profile == "performance");
  CHECK(scalar.reason == RecommendationReason::Isa);
  CHECK(scalar.rules.size() == 2);
  CHECK(scalar.fallback == "performance");
}

TEST_CASE("hardware facts round the RAM to a tenth and name the widest vector ISA")
{
  HardwareProfile profile;
  profile.physicalCores = 8;
  profile.logicalThreads = 16;
  profile.ramTotalMb = 31485;
  profile.avx2 = true;
  profile.videoAccel = VideoAccel::Vaapi;
  const auto facts = hardwareFactsOf(profile);
  CHECK(facts.cores == 8);
  CHECK(facts.threads == 16);
  CHECK(facts.ramGb == doctest::Approx(30.7));
  CHECK(facts.isa == CpuIsa::Avx2);
  CHECK(facts.vectorIsa());
  CHECK((facts.gpu == VideoAccel::Vaapi));
  profile.avx512 = true;
  CHECK(hardwareFactsOf(profile).isa == CpuIsa::Avx512);
}

TEST_CASE("the shipped profile file parses, in owner display order")
{
  const auto catalog = loadProfileFile(ARGUS_SETTINGS_PROFILES_FILE).value_or(ProfileCatalog{});
  REQUIRE(catalog.profiles.size() == 3);
  CHECK(catalog.profiles[0].id == "performance");
  CHECK(catalog.profiles[1].id == "balanced");
  CHECK(catalog.profiles[2].id == "quality");
  CHECK(catalog.fallback == "performance");
  CHECK(catalog.owners() == std::vector<std::string>{"llm", "tts", "vlm"});
  for (const auto& profile : catalog.profiles) {
    const auto offset = profile.id == "quality" ? 1U : 0U;
    REQUIRE(profile.owners.size() == 2 + offset);
    CHECK(profile.owners[offset].owner == "tts");
    CHECK(profile.owners[offset + 1].owner == "vlm");
  }
  const auto* quality = catalog.find("quality");
  REQUIRE(quality != nullptr);
  CHECK(quality->owners[0].owner == "llm");
  CHECK(quality->owners[0].changes.empty());
  REQUIRE(quality->owners[0].withCapability.size() == 1);
  CHECK(quality->owners[0].withCapability[0].capability == "gpu");
  CHECK(quality->owners[0].withCapability[0].change.key == "llm.gpu_layers");
  CHECK(quality->owners[0].withCapability[0].change.value == "999");
  REQUIRE(quality->owners[2].withCapability.size() == 1);
  CHECK(quality->owners[2].withCapability[0].change.key == "vision.gpu_layers");
  CHECK(catalog.find("balanced")->owners[0].withCapability.empty());
  const auto& tts = quality->owners[1].changes;
  CHECK(std::ranges::find(tts, std::string("tts.pocket_variant_es"), &SettingChange::key)->value == "quality");
  CHECK(std::ranges::find(tts, std::string("tts.pocket_voice_es"), &SettingChange::key)->value == "jean");
  CHECK_FALSE(loadProfileFile("/nonexistent/argus-profiles.json").has_value());
}

TEST_CASE("a malformed profile file is refused with the place of the problem")
{
  const auto problemOf = [](const std::string& text) { return parseProfileCatalog(parsed(text)).problem; };
  const std::string recommendation = R"("recommendation": {"rules": [], "fallback": "a"})";

  CHECK(problemOf(R"({"profiles": [], )" + recommendation + "}").find("profiles") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"gateway": {"x.y": "1"}}}], )" +
                  recommendation + "}")
            .find("profiles[0].owners.gateway") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"tts": {"tts.speed": 1}}}], )" +
                  recommendation + "}")
            .find("tts.speed") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "A b", "labelKey": "a", "owners": {"tts": {"tts.speed": "1"}}}], )" +
                  recommendation + "}")
            .find("profiles[0].id") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"tts": {"tts.speed": "1"}}},
                                    {"id": "a", "labelKey": "b", "owners": {"tts": {"tts.speed": "1"}}}], )" +
                  recommendation + "}")
            .find("repeats") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"tts": {"tts.speed": "1"}}}],
                      "recommendation": {"rules": [{"profile": "b", "minCores": 1, "minRamGb": 1}], "fallback": "a"}})")
            .find("rules[0].profile") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"tts": {"tts.speed": "1"}}}],
                      "recommendation": {"rules": [], "fallback": "z"}})")
            .find("fallback") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", "owners": {"tts": {"tts.speed": "1"}}}], )" +
                  recommendation + "}")
            .empty());
}

TEST_CASE("the listing and the apply answer through real owners, and a host-only choice never blocks the rest")
{
  loadConfig();
  Owner tts("tts", ttsSpecs());
  tts.registry.describeChoices([](const SettingSpec& spec) {
    if (spec.key != "tts.pocket_voice_es")
      return std::vector<ChoiceState>{};
    return std::vector<ChoiceState>{
        {.choice = "jean", .availability = ChoiceAvailability::HostOnly, .sizeMb = 30, .hostCommand = "provision --voice es:jean"}};
  });
  const Owner vlm("vlm", vlmSpecs());
  const SettingsGatewayService gateway({.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret},
                                                   {.name = "vlm", .target = vlm.target(), .credential = kSecret}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms}});
  const SettingsProfileService profiles(
      {.gateway = gateway, .catalog = catalogOf({qualityProfile()}), .hardware = hardware(8, 30.7, CpuIsa::Avx2)});

  const auto overview = profiles.overview();
  REQUIRE(overview.profiles.size() == 1);
  CHECK(overview.recommendation.profile == "quality");
  const auto& preview = overview.profiles[0];
  CHECK_FALSE(preview.current);
  REQUIRE(preview.owners.size() == 2);
  CHECK(preview.owners[0].changes[0].key == "tts.pocket_variant_es");
  CHECK_FALSE(preview.owners[0].changes[0].changed);
  CHECK(preview.owners[0].changes[1].install.value_or(ChoiceState{}).availability == ChoiceAvailability::HostOnly);
  CHECK(preview.owners[1].changes[0].from == "384");

  const auto listing = ResponseListProfilesDto{.overview = overview}.toJson();
  CHECK(listing["recommendation"]["hardware"]["isa"].asString() == "avx2");
  CHECK(listing["recommendation"]["missed"].isNull());
  REQUIRE(listing["recommendation"]["rules"].size() == 2);
  CHECK(listing["recommendation"]["rules"][1]["profile"].asString() == "balanced");
  CHECK(listing["recommendation"]["rules"][1]["minCores"].asInt() == 4);
  CHECK(listing["recommendation"]["fallback"].asString() == "performance");
  CHECK(listing["profiles"][0]["owners"][0]["changes"][1]["install"]["availability"].asString() == "hostOnly");

  const auto outcome = profiles.apply({.profile = "quality", .userId = 1});
  CHECK(outcome.profile == "quality");
  REQUIRE(outcome.owners.size() == 2);
  CHECK(resultOf(outcome.owners[0], "tts.pocket_variant_es").status == ProfileKeyStatus::Unchanged);
  CHECK(resultOf(outcome.owners[0], "tts.pocket_voice_es").reason == SettingRejectionReason::NotInstalled);
  CHECK(resultOf(outcome.owners[1], "vision.max_input_px").status == ProfileKeyStatus::Applied);
  const auto vlmCatalog = outcome.owners[1].catalog.value_or(OwnerCatalog{});
  REQUIRE(vlmCatalog.settings.size() == 1);
  CHECK(vlmCatalog.settings[0].value == "512");
  CHECK(ConfigService::getInt("vision.max_input_px") == 512);
  CHECK(ConfigService::getString("tts.pocket_voice_es") == "lola");

  const auto answer = ResponseApplyProfileDto{.outcome = outcome}.toJson();
  CHECK(answer["summary"]["applied"].asInt() == 1);
  CHECK(answer["summary"]["unchanged"].asInt() == 1);
  CHECK(answer["summary"]["rejected"].asInt() == 1);
  CHECK(answer["summary"]["unreachable"].asInt() == 0);
  CHECK(answer["owners"][0]["results"][1]["reason"].asString() == "notInstalled");
  CHECK(answer["owners"][1]["catalog"]["settings"][0]["value"].asString() == "512");

  CHECK(statusOf([&profiles] { (void)profiles.apply({.profile = "nope", .userId = 1}); }) == 404);
  std::filesystem::remove(configPath());
}

TEST_CASE("an unconfigured owner is unreachable and a missing profile file answers 503")
{
  loadConfig();
  const Owner vlm("vlm", vlmSpecs());
  const SettingsGatewayService gateway({.owners = {{.name = "vlm", .target = vlm.target(), .credential = kSecret}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms}});
  const SettingsProfileService profiles(
      {.gateway = gateway, .catalog = catalogOf({qualityProfile()}), .hardware = hardware(4, 8, CpuIsa::Avx2)});

  const auto outcome = profiles.apply({.profile = "quality", .userId = 1});
  CHECK_FALSE(outcome.owners[0].reachable);
  CHECK(resultOf(outcome.owners[0], "tts.pocket_voice_es").status == ProfileKeyStatus::Unreachable);
  CHECK(resultOf(outcome.owners[1], "vision.max_input_px").status == ProfileKeyStatus::Applied);
  CHECK(profiles.overview().recommendation.profile == "balanced");

  const SettingsProfileService missing({.gateway = gateway, .catalog = std::nullopt, .hardware = hardware(4, 8, CpuIsa::Avx2)});
  CHECK(statusOf([&missing] { (void)missing.overview(); }) == 503);
  CHECK(statusOf([&missing] { (void)missing.apply({.profile = "quality", .userId = 1}); }) == 503);
  std::filesystem::remove(configPath());
}

namespace
{
SettingSpec gpuSpec(const std::string& key)
{
  return {.key = key,
          .group = "engine",
          .type = SettingType::Integer,
          .level = SettingLevel::Advanced,
          .apply = SettingApply::Restart,
          .range = {.min = -1, .max = 999, .step = 1},
          .choices = {},
          .fallback = "-1",
          .unit = "layers"};
}

OwnerCatalog vlmCatalog(const std::string& inputPx, const std::string& gpuLayers, bool gpu)
{
  OwnerCatalog catalog{.service = "vlm",
                       .reachable = true,
                       .settings = {entry(integerSpec("vision.max_input_px"), inputPx),
                                    entry(gpuSpec("vision.gpu_layers"), gpuLayers)},
                       .configured = true,
                       .configFile = "/srv/argus/config.vlm.toml",
                       .profile = ProfileMarker{},
                       .capabilities = {}};
  if (gpu)
    catalog.capabilities.emplace_back("gpu");
  return catalog;
}

SettingsProfile gpuQualityProfile()
{
  auto profile = qualityProfile();
  profile.owners[1].withCapability.push_back(
      {.capability = "gpu", .change = {.key = "vision.gpu_layers", .value = "999"}});
  profile.owners.insert(profile.owners.begin(),
                        ProfileOwnerChanges{.owner = "llm",
                                            .changes = {},
                                            .withCapability = {{.capability = "gpu",
                                                                .change = {.key = "llm.gpu_layers", .value = "999"}}}});
  return profile;
}

std::vector<SettingSpec> vlmGpuSpecs()
{
  return {integerSpec("vision.max_input_px"), gpuSpec("vision.gpu_layers")};
}

void loadVlmConfig(const std::string& body)
{
  std::ofstream(configPath()) << body;
  ConfigService::load(configPath().string());
}
}

TEST_CASE("a capability key joins the profile only where the owner reports the capability")
{
  const auto profile = gpuQualityProfile();
  const auto cpu = settings_profile::preview(profile, {vlmCatalog("512", "-1", false)});
  REQUIRE(cpu.owners.size() == 2);
  CHECK(cpu.owners[0].service == "tts");
  CHECK(cpu.owners[1].changes.size() == 1);

  const auto gpu = settings_profile::preview(profile, {vlmCatalog("512", "-1", true)});
  REQUIRE(gpu.owners[1].changes.size() == 2);
  CHECK(gpu.owners[1].changes[1].key == "vision.gpu_layers");
  CHECK(gpu.owners[1].changes[1].changed);
  CHECK(gpu.owners[1].changes[1].apply == SettingApply::Restart);

  SettingsProfile vlmOnly{.id = "quality", .labelKey = "quality", .owners = {profile.owners[0], profile.owners[2]}};
  vlmOnly.owners[1].changes = {{.key = "vision.max_input_px", .value = "512"}};
  CHECK_FALSE(settings_profile::preview(vlmOnly, {vlmCatalog("512", "-1", true)}).current);
  CHECK(settings_profile::preview(vlmOnly, {vlmCatalog("512", "999", true)}).current);
  CHECK(settings_profile::preview(vlmOnly, {vlmCatalog("512", "-1", false)}).current);
  CHECK(settings_profile::plan(vlmOnly, {vlmCatalog("512", "-1", false)}).size() == 1);
}

TEST_CASE("a capability block is validated like the owners it names")
{
  const auto problemOf = [](const std::string& text) { return parseProfileCatalog(parsed(text)).problem; };
  const std::string recommendation = R"("recommendation": {"rules": [], "fallback": "a"})";
  const std::string owners = R"("owners": {"vlm": {"vision.max_input_px": "512"}})";
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", )" + owners +
                  R"(, "withCapability": {"gpu": {"vlm": {"vision.gpu_layers": "999"}}}}], )" + recommendation + "}")
            .empty());
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", )" + owners +
                  R"(, "withCapability": {"gpu": {"vlm": {"vision.max_input_px": "640"}}}}], )" + recommendation + "}")
            .find("twice") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", )" + owners +
                  R"(, "withCapability": {"GPU!": {"vlm": {"vision.gpu_layers": "999"}}}}], )" + recommendation + "}")
            .find("capability") != std::string::npos);
  CHECK(problemOf(R"({"profiles": [{"id": "a", "labelKey": "a", )" + owners +
                  R"(, "withCapability": {"gpu": {"gateway": {"x.y": "1"}}}}], )" + recommendation + "}")
            .find("gateway") != std::string::npos);
}

TEST_CASE("the first run touches only keys still at their default, never installs, and skips what already matches")
{
  auto profile = gpuQualityProfile();
  const auto fresh = settings_profile::firstRunPlan(profile, vlmCatalog("384", "-1", true));
  REQUIRE(fresh.keys.size() == 2);
  CHECK(fresh.keys[0].result.key == "vision.max_input_px");
  CHECK(fresh.keys[0].result.to == "512");
  CHECK(fresh.keys[1].result.key == "vision.gpu_layers");
  CHECK(fresh.keys[1].send);

  const auto handTuned = settings_profile::firstRunPlan(profile, vlmCatalog("640", "0", true));
  CHECK(handTuned.keys.empty());

  const auto noGpu = settings_profile::firstRunPlan(profile, vlmCatalog("384", "-1", false));
  REQUIRE(noGpu.keys.size() == 1);
  CHECK(noGpu.keys[0].result.key == "vision.max_input_px");

  const auto tts = settings_profile::firstRunPlan(profile, ttsCatalog("quality", "lola"));
  CHECK(tts.keys.empty());
  auto installable = ttsCatalog("fast", "lola");
  installable.settings[0].spec.fallback = "fast";
  SettingsProfile slow{.id = "quality", .labelKey = "quality", .owners = {{.owner = "tts", .changes = {{.key = "tts.pocket_variant_es", .value = "quality"}}, .withCapability = {}}}};
  installable.settings[0].choiceStates[1].availability = ChoiceAvailability::Installable;
  CHECK(settings_profile::firstRunPlan(slow, installable).keys.empty());
  installable.settings[0].choiceStates[1].availability = ChoiceAvailability::Installed;
  CHECK(settings_profile::firstRunPlan(slow, installable).keys.size() == 1);
}

TEST_CASE("undoing the recommendation restores only the keys that still hold what it set")
{
  const auto profiles = catalogOf({gpuQualityProfile()});
  auto applied = vlmCatalog("512", "0", true);
  applied.profile = ProfileMarker{.id = "quality",
                                  .origin = ProfileOrigin::Recommended,
                                  .appliedAt = 1759500000,
                                  .keys = {"vision.max_input_px", "vision.gpu_layers"}};
  const auto plan = settings_profile::revertPlan(profiles, applied);
  REQUIRE(plan.keys.size() == 1);
  CHECK(plan.keys[0].result.key == "vision.max_input_px");
  CHECK(plan.keys[0].result.to == "384");

  auto chosen = applied;
  chosen.profile->origin = ProfileOrigin::Owner;
  CHECK(settings_profile::revertPlan(profiles, chosen).keys.empty());

  const auto state = settings_profile::firstRunState({applied, vlmCatalog("384", "-1", false)});
  REQUIRE(state.has_value());
  CHECK(state->profile == "quality");
  CHECK(state->origin == ProfileOrigin::Recommended);
  CHECK(state->appliedAt == 1759500000);
  REQUIRE(state->owners.size() == 1);
  CHECK(state->owners[0].keys.size() == 2);
  CHECK_FALSE(settings_profile::firstRunState({vlmCatalog("384", "-1", false)}).has_value());
}

TEST_CASE("a profile marker rides every write with the keys applied so far, and alone when nothing changed")
{
  auto plans = settings_profile::plan(gpuQualityProfile(), {vlmCatalog("384", "-1", true)});
  REQUIRE(plans.size() == 2);
  CHECK_FALSE(plans[0].reachable);
  plans[0].marker = ProfileMarker{.id = "quality", .origin = ProfileOrigin::Owner, .appliedAt = 7, .keys = {}};
  plans[1].marker = ProfileMarker{.id = "quality", .origin = ProfileOrigin::Owner, .appliedAt = 7, .keys = {}};
  ProfileApplication application(plans);
  auto writes = application.pendingWrites();
  REQUIRE(writes.size() == 1);
  CHECK(writes[0].owner == "vlm");
  REQUIRE(writes[0].marker.has_value());
  CHECK(writes[0].marker->keys == std::vector<std::string>{"vision.max_input_px", "vision.gpu_layers"});
  application.record({{.owner = "vlm",
                       .reachable = true,
                       .applied = {"vision.max_input_px", "vision.gpu_layers"},
                       .rejected = {},
                       .catalog = std::nullopt,
                       .markerRecorded = true}});
  CHECK(application.markerWrites().empty());

  auto unchanged = settings_profile::plan(gpuQualityProfile(), {vlmCatalog("512", "999", true)});
  unchanged[1].marker = ProfileMarker{.id = "quality", .origin = ProfileOrigin::Owner, .appliedAt = 7, .keys = {}};
  const ProfileApplication settled(unchanged);
  CHECK(settled.pendingWrites().empty());
  const auto markers = settled.markerWrites();
  REQUIRE(markers.size() == 1);
  CHECK(markers[0].owner == "vlm");
  CHECK(markers[0].changes.empty());
  CHECK(markers[0].marker->keys.empty());
}

TEST_CASE("the first run applies the recommendation once, through a real owner, and the owner can undo it")
{
  loadVlmConfig("[vision]\nmax_input_px = 384\ngpu_layers = -1\n");
  Owner vlm("vlm", vlmGpuSpecs());
  vlm.registry.declareCapability("gpu");
  const SettingsGatewayService gateway({.owners = {{.name = "vlm", .target = vlm.target(), .credential = kSecret}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms},
                                        .unconfigured = {"tts"}});
  const auto catalog = catalogOf({gpuQualityProfile()});

  FirstRunService firstRun({.gateway = gateway,
                            .catalog = catalog,
                            .hardware = hardware(8, 30.7, CpuIsa::Avx2),
                            .config = {.enabled = true, .interval = 30s}});
  const auto pass = firstRun.runOnce();
  CHECK(pass.waiting.empty());
  CHECK(std::ranges::find(pass.settled, std::string("vlm")) != pass.settled.end());
  CHECK(ConfigService::getInt("vision.max_input_px") == 512);
  CHECK(ConfigService::getInt("vision.gpu_layers") == 999);
  CHECK(ConfigService::getString("settings_profile.origin") == "recommended");

  CHECK(ConfigService::hasKey("settings_profile.keys"));
  REQUIRE(vlm.registry.update({{.key = "vision.gpu_layers", .value = "0"}}).rejected.empty());
  FirstRunService again({.gateway = gateway,
                         .catalog = catalog,
                         .hardware = hardware(8, 30.7, CpuIsa::Avx2),
                         .config = {.enabled = true, .interval = 30s}});
  CHECK(again.runOnce().waiting.empty());
  CHECK(ConfigService::getInt("vision.gpu_layers") == 0);

  const SettingsProfileService profiles(
      {.gateway = gateway, .catalog = catalog, .hardware = hardware(8, 30.7, CpuIsa::Avx2)});
  const auto overview = profiles.overview();
  REQUIRE(overview.firstRun.has_value());
  CHECK(overview.firstRun->profile == "quality");
  const auto listing = ResponseListProfilesDto{.overview = overview}.toJson();
  CHECK(listing["firstRun"]["state"].asString() == "applied");
  CHECK(listing["firstRun"]["owners"][0]["service"].asString() == "vlm");

  const auto undone = profiles.revertRecommended({.userId = 1});
  REQUIRE(undone.owners.size() == 1);
  CHECK(resultOf(undone.owners[0], "vision.max_input_px").status == ProfileKeyStatus::Applied);
  CHECK(ConfigService::getInt("vision.max_input_px") == 384);
  CHECK(ConfigService::getInt("vision.gpu_layers") == 0);
  CHECK(ConfigService::getString("settings_profile.origin") == "reverted");
  CHECK(ResponseListProfilesDto{.overview = profiles.overview()}.toJson()["firstRun"]["state"].asString() == "reverted");
  CHECK(statusOf([&profiles] { (void)profiles.revertRecommended({.userId = 1}); }) == 404);

  CHECK(firstRun.runOnce().settled.empty());
  CHECK(ConfigService::getInt("vision.max_input_px") == 384);
  std::filesystem::remove(configPath());
}

TEST_CASE("a disabled first run never starts and an idle one is drained")
{
  const SettingsGatewayService gateway({.owners = {}, .timeouts = {.list = 1500ms, .update = 1500ms}, .unconfigured = {}});
  FirstRunService disabled({.gateway = gateway,
                            .catalog = catalogOf({qualityProfile()}),
                            .hardware = hardware(8, 30.7, CpuIsa::Avx2),
                            .config = {.enabled = false, .interval = 30s}});
  disabled.start();
  CHECK(disabled.drained());
  FirstRunService running({.gateway = gateway,
                           .catalog = catalogOf({qualityProfile()}),
                           .hardware = hardware(8, 30.7, CpuIsa::Avx2),
                           .config = {.enabled = true, .interval = 30s}});
  running.start();
  CHECK_FALSE(running.drained());
  running.requestStop();
  for (int attempt = 0; attempt < 50 && !running.drained(); ++attempt)
    std::this_thread::sleep_for(20ms);
  CHECK(running.drained());
}
