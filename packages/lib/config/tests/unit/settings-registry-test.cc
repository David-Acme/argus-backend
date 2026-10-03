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
