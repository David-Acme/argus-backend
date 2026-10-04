#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string writeConfig(const std::string& body)
{
  const std::string path = "/tmp/settings-registry-test.toml";
  std::ofstream out(path);
  out << body;
  return path;
}

std::string readFile(const std::string& path)
{
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<SettingSpec> llmSpecs()
{
  return {
      {.key = "llm.temperature",
       .group = "sampling",
       .type = SettingType::Decimal,
       .level = SettingLevel::Basic,
       .apply = SettingApply::Live,
       .range = {.min = 0, .max = 2, .step = 0.05},
       .choices = {},
       .fallback = "0.85"},
      {.key = "llm.max_tokens",
       .group = "sampling",
       .type = SettingType::Integer,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Live,
       .range = {.min = 16, .max = 4096, .step = 16},
       .choices = {},
       .fallback = "256"},
      {.key = "llm.kv_type",
       .group = "engine",
       .type = SettingType::Choice,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Restart,
       .range = {},
       .choices = {"auto", "f16", "q8_0"},
       .fallback = "auto"},
      {.key = "llm.swa_full",
       .group = "engine",
       .type = SettingType::Toggle,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Restart,
       .range = {},
       .choices = {},
       .fallback = "false"},
  };
}

const SettingEntry& entryOf(const std::vector<SettingEntry>& entries, const std::string& key)
{
  for (const auto& entry : entries)
    if (entry.spec.key == key)
      return entry;
  throw std::out_of_range(key);
}
}

TEST_CASE("list reports the file's value, or the fallback when the key is absent")
{
  const auto path = writeConfig("[llm]\n# sampling\ntemperature = 0.5\nmodel_path = \"secret/path.gguf\"\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());

  const auto entries = registry.list();
  CHECK(entries.size() == 4);
  CHECK(entryOf(entries, "llm.temperature").value == "0.5");
  CHECK(entryOf(entries, "llm.max_tokens").value == "256");
  CHECK(entryOf(entries, "llm.kv_type").value == "auto");
  CHECK(entryOf(entries, "llm.swa_full").value == "false");
  for (const auto& entry : entries)
    CHECK(entry.spec.key != "llm.model_path");
  std::remove(path.c_str());
}

TEST_CASE("an update persists canonical values, keeps comments and tells the listeners")
{
  const auto path = writeConfig("[llm]\n# sampling\ntemperature = 0.5\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  std::vector<std::string> notified;
  registry.onChange([&notified](const std::vector<std::string>& keys) { notified = keys; });

  const auto result = registry.update({{.key = "llm.temperature", .value = "0.70"},
                                       {.key = "llm.max_tokens", .value = "512"},
                                       {.key = "llm.swa_full", .value = "true"}});

  CHECK(result.rejected.empty());
  CHECK(result.applied.size() == 3);
  CHECK(notified == result.applied);
  CHECK(ConfigService::getDouble("llm.temperature") == doctest::Approx(0.7));
  const auto written = readFile(path);
  CHECK(written.find("temperature = 0.7\n") != std::string::npos);
  CHECK(written.find("max_tokens = 512") != std::string::npos);
  CHECK(written.find("# sampling") != std::string::npos);
  CHECK(entryOf(registry.list(), "llm.temperature").value == "0.7");
  std::remove(path.c_str());
}

TEST_CASE("one invalid change rejects the whole update and writes nothing")
{
  const auto path = writeConfig("[llm]\ntemperature = 0.5\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  bool notified = false;
  registry.onChange([&notified](const std::vector<std::string>&) { notified = true; });

  const auto result = registry.update({{.key = "llm.temperature", .value = "0.6"},
                                       {.key = "llm.max_tokens", .value = "99999"},
                                       {.key = "llm.kv_type", .value = "q4"},
                                       {.key = "llm.model_path", .value = "/etc/passwd"},
                                       {.key = "llm.swa_full", .value = "yes"}});

  CHECK(result.applied.empty());
  REQUIRE(result.rejected.size() == 4);
  CHECK(result.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[1].reason == SettingRejectionReason::NotAChoice);
  CHECK(result.rejected[2].reason == SettingRejectionReason::Unknown);
  CHECK(result.rejected[3].reason == SettingRejectionReason::Invalid);
  CHECK_FALSE(notified);
  CHECK(ConfigService::getDouble("llm.temperature") == doctest::Approx(0.5));
  CHECK(readFile(path).find("model_path") == std::string::npos);
  std::remove(path.c_str());
}

TEST_CASE("decimal and integer parsing refuse partial and non-finite input")
{
  const auto path = writeConfig("[llm]\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  for (const std::string bad : {"", "0.5x", "nan", "inf", " 0.5", "1e400"})
    CHECK_FALSE(registry.update({{.key = "llm.temperature", .value = bad}}).rejected.empty());
  for (const std::string bad : {"12.5", "3000000000", "+", "64 "})
    CHECK_FALSE(registry.update({{.key = "llm.max_tokens", .value = bad}}).rejected.empty());
  std::remove(path.c_str());
}

TEST_CASE("a catalog with a duplicate key, a bare key or a bad fallback is refused")
{
  auto duplicate = llmSpecs();
  duplicate.push_back(duplicate.front());
  CHECK_THROWS_AS(SettingsRegistry{duplicate}, std::invalid_argument);

  auto bare = llmSpecs();
  bare.front().key = "temperature";
  CHECK_THROWS_AS(SettingsRegistry{bare}, std::invalid_argument);

  auto fallback = llmSpecs();
  fallback.front().fallback = "9";
  CHECK_THROWS_AS(SettingsRegistry{fallback}, std::invalid_argument);
}

TEST_CASE("a choice setting carries what its owner says about each choice, and nothing else does")
{
  const auto path = writeConfig("[llm]\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  CHECK(entryOf(registry.list(), "llm.kv_type").choiceStates.empty());

  std::vector<std::string> asked;
  registry.describeChoices([&asked](const SettingSpec& spec) {
    asked.push_back(spec.key);
    return std::vector<ChoiceState>{
        {.choice = "f16", .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = ""},
        {.choice = "q8_0", .availability = ChoiceAvailability::Installable, .sizeMb = 219.3, .hostCommand = "run q8"},
        {.choice = "q4", .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = ""}};
  });
  const auto entries = registry.list();
  CHECK(asked == std::vector<std::string>{"llm.kv_type"});
  const auto& states = entryOf(entries, "llm.kv_type").choiceStates;
  REQUIRE(states.size() == 2);
  CHECK(states[1].choice == "q8_0");
  CHECK(states[1].availability == ChoiceAvailability::Installable);
  CHECK(states[1].sizeMb == doctest::Approx(219.3));
  CHECK(states[1].hostCommand == "run q8");
  CHECK(entryOf(entries, "llm.temperature").choiceStates.empty());
  std::remove(path.c_str());
}

TEST_CASE("a choice that only the host can install is refused; one the owner can install is applied")
{
  const auto path = writeConfig("[llm]\nkv_type = \"q8_0\"\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  auto availability = ChoiceAvailability::HostOnly;
  registry.describeChoices([&availability](const SettingSpec&) {
    return std::vector<ChoiceState>{
        {.choice = "f16", .availability = availability, .sizeMb = 12, .hostCommand = "provision f16"},
        {.choice = "q8_0", .availability = ChoiceAvailability::HostOnly, .sizeMb = 0, .hostCommand = "provision q8"}};
  });
  std::vector<std::string> notified;
  registry.onChange([&notified](const std::vector<std::string>& keys) { notified = keys; });

  const auto refused = registry.update({{.key = "llm.kv_type", .value = "f16"}});
  REQUIRE(refused.rejected.size() == 1);
  CHECK(refused.rejected[0].reason == SettingRejectionReason::NotInstalled);
  CHECK(notified.empty());
  CHECK(ConfigService::getString("llm.kv_type") == "q8_0");

  CHECK(registry.update({{.key = "llm.kv_type", .value = "q8_0"}}).rejected.empty());
  CHECK(notified == std::vector<std::string>{"llm.kv_type"});

  for (const auto state : {ChoiceAvailability::Installable, ChoiceAvailability::Failed, ChoiceAvailability::Installing}) {
    availability = state;
    CHECK(registry.update({{.key = "llm.kv_type", .value = "f16"}}).rejected.empty());
    CHECK(ConfigService::getString("llm.kv_type") == "f16");
    CHECK(registry.update({{.key = "llm.kv_type", .value = "auto"}}).rejected.empty());
  }
  std::remove(path.c_str());
}

TEST_CASE("a restart key reads as pending once the file holds a value the process did not boot with")
{
  const auto path = writeConfig("[llm]\ntemperature = 0.5\nkv_type = \"auto\"\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  CHECK_FALSE(entryOf(registry.list(), "llm.kv_type").pendingRestart);

  REQUIRE(registry.update({{.key = "llm.kv_type", .value = "f16"}, {.key = "llm.temperature", .value = "0.6"}})
              .rejected.empty());
  auto entries = registry.list();
  CHECK(entryOf(entries, "llm.kv_type").pendingRestart);
  CHECK_FALSE(entryOf(entries, "llm.temperature").pendingRestart);

  REQUIRE(registry.update({{.key = "llm.kv_type", .value = "auto"}}).rejected.empty());
  CHECK_FALSE(entryOf(registry.list(), "llm.kv_type").pendingRestart);

  REQUIRE(registry.update({{.key = "llm.kv_type", .value = "q8_0"}}).rejected.empty());
  SettingsRegistry rebooted(llmSpecs());
  CHECK_FALSE(entryOf(rebooted.list(), "llm.kv_type").pendingRestart);
  std::remove(path.c_str());
}

TEST_CASE("the profile marker is absent until recorded, then persists in its own section beside the comments")
{
  const auto path = writeConfig("# owner file\n[llm]\ntemperature = 0.5\n");
  ConfigService::load(path);
  SettingsRegistry registry(llmSpecs());
  const auto empty = registry.profileMarker();
  CHECK(empty.id.empty());
  CHECK(empty.origin == ProfileOrigin::None);
  CHECK(empty.keys.empty());

  REQUIRE(registry.recordProfile({.id = "quality",
                                  .origin = ProfileOrigin::Recommended,
                                  .appliedAt = 1759500000,
                                  .keys = {"llm.temperature", "llm.kv_type"}}));
  const auto written = readFile(path);
  CHECK(written.find("# owner file") != std::string::npos);
  CHECK(written.find("[settings_profile]") != std::string::npos);
  CHECK(written.find("origin = \"recommended\"") != std::string::npos);

  ConfigService::load(path);
  const auto marker = SettingsRegistry(llmSpecs()).profileMarker();
  CHECK(marker.id == "quality");
  CHECK(marker.origin == ProfileOrigin::Recommended);
  CHECK(marker.appliedAt == 1759500000);
  CHECK(marker.keys == std::vector<std::string>{"llm.temperature", "llm.kv_type"});
  CHECK(entryOf(registry.list(), "llm.temperature").value == "0.5");

  CHECK_FALSE(registry.recordProfile({.id = "bad\nid", .origin = ProfileOrigin::Owner, .appliedAt = 1, .keys = {}}));
  CHECK_FALSE(registry.recordProfile({.id = "quality", .origin = ProfileOrigin::None, .appliedAt = 1, .keys = {}}));
  CHECK_FALSE(registry.recordProfile({.id = "quality", .origin = ProfileOrigin::Owner, .appliedAt = -1, .keys = {}}));
  std::remove(path.c_str());
}

TEST_CASE("capabilities are declared once each and a spec carries its unit")
{
  const auto path = writeConfig("[llm]\n");
  ConfigService::load(path);
  auto specs = llmSpecs();
  specs[1].unit = "tokens";
  SettingsRegistry registry(specs);
  registry.declareCapability("gpu");
  registry.declareCapability("gpu");
  CHECK(registry.capabilities() == std::vector<std::string>{"gpu"});
  CHECK(entryOf(registry.list(), "llm.max_tokens").spec.unit == "tokens");
  CHECK(entryOf(registry.list(), "llm.temperature").spec.unit.empty());
  CHECK(profileOriginFromString(profileOriginToString(ProfileOrigin::Reverted)) == ProfileOrigin::Reverted);
  CHECK(profileOriginFromString("unknown") == ProfileOrigin::None);
  std::remove(path.c_str());
}

TEST_CASE("a numeric key without a declared unit takes the one its name spells, and a declared one wins")
{
  const auto path = writeConfig("[x]\n");
  ConfigService::load(path);
  const auto numeric = [](const std::string& key, const std::string& unit) {
    return SettingSpec{.key = key,
                       .group = "g",
                       .type = SettingType::Integer,
                       .level = SettingLevel::Advanced,
                       .apply = SettingApply::Live,
                       .range = {},
                       .choices = {},
                       .fallback = "0",
                       .unit = unit};
  };
  SettingsRegistry registry({numeric("x.wait_ms", ""), numeric("x.window_s", ""), numeric("x.alarm_seconds", ""),
                             numeric("x.steps_low", ""), numeric("x.context_size", ""), numeric("x.gpu_layers", ""),
                             numeric("x.batch_threads", ""), numeric("x.top_k", ""), numeric("x.speed_ms", "x"),
                             {.key = "x.prompt_ms",
                              .group = "g",
                              .type = SettingType::Text,
                              .level = SettingLevel::Advanced,
                              .apply = SettingApply::Live,
                              .range = {},
                              .choices = {},
                              .fallback = "",
                              .unit = ""}});
  const auto entries = registry.list();
  CHECK(entryOf(entries, "x.wait_ms").spec.unit == "ms");
  CHECK(entryOf(entries, "x.window_s").spec.unit == "s");
  CHECK(entryOf(entries, "x.alarm_seconds").spec.unit == "s");
  CHECK(entryOf(entries, "x.steps_low").spec.unit == "steps");
  CHECK(entryOf(entries, "x.context_size").spec.unit == "tokens");
  CHECK(entryOf(entries, "x.gpu_layers").spec.unit == "layers");
  CHECK(entryOf(entries, "x.batch_threads").spec.unit == "threads");
  CHECK(entryOf(entries, "x.top_k").spec.unit.empty());
  CHECK(entryOf(entries, "x.speed_ms").spec.unit == "x");
  CHECK(entryOf(entries, "x.prompt_ms").spec.unit.empty());
  std::remove(path.c_str());
}
