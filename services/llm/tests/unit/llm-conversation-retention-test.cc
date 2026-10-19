#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>
#include <feature/conversation/services/conversation-service.hxx>
#include <feature/settings/llm-settings.hxx>

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif

namespace
{
constexpr const char* kRetentionKey = "conversation.retention_days";
constexpr int64_t kUser = 31;
constexpr int64_t kNow = 100000000;
constexpr int64_t kDay = 86400;

void loadScratchConfig()
{
  const auto path = std::filesystem::temp_directory_path() / "llm-conversation-retention-test.toml";
  {
    std::ofstream config(path, std::ios::trunc);
    config << "[conversation]\nidle_seconds = 900\nretention_days = 30\nswitch_budget_ms = 100\n";
  }
  ConfigService::load(path.string());
}

SettingsUpdateResult setRetention(const std::string& value)
{
  SettingsRegistry registry(llmSettingsCatalog());
  return registry.update({{.key = kRetentionKey, .value = value}});
}

struct Database
{
  sqlite3* handle{nullptr};

  Database()
  {
    sqlite3_open(":memory:", &handle);
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    std::ifstream file(ARGUS_TEST_MEMORY_SCHEMA);
    const std::string schema((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    sqlite3_exec(handle, schema.c_str(), nullptr, nullptr, nullptr);
  }

  ~Database() { sqlite3_close(handle); }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
};

void recordAgo(const ConversationService& service, int64_t daysAgo, const std::string& content)
{
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone,
                              .content = content,
                              .at = kNow - daysAgo * kDay}));
}
}

TEST_CASE("the retention setting is an owner privacy setting from 1 to 365 days, 30 by default")
{
  const auto catalog = llmSettingsCatalog();
  const auto found = std::find_if(catalog.begin(), catalog.end(),
                                  [](const SettingSpec& spec) { return spec.key == kRetentionKey; });
  REQUIRE(found != catalog.end());
  CHECK(found->group == "privacy");
  CHECK(found->type == SettingType::Integer);
  CHECK(found->level == SettingLevel::Basic);
  CHECK(found->apply == SettingApply::Live);
  CHECK(found->range.min == 1);
  CHECK(found->range.max == 365);
  CHECK(found->fallback == "30");
}

TEST_CASE("the retention setting accepts its two bounds and the default")
{
  loadScratchConfig();
  for (const char* accepted : {"1", "30", "365"}) {
    const auto result = setRetention(accepted);
    CHECK_MESSAGE(result.rejected.empty(), accepted);
    CHECK(result.applied == std::vector<std::string>{kRetentionKey});
  }
}

TEST_CASE("the retention setting refuses values outside its range and values that are not whole days")
{
  loadScratchConfig();
  for (const char* outOfRange : {"0", "366", "-7"}) {
    const auto result = setRetention(outOfRange);
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected.front().reason == SettingRejectionReason::OutOfRange);
    CHECK(result.applied.empty());
  }
  for (const char* notWhole : {"2.5", "no limit", "", "forever"}) {
    const auto result = setRetention(notWhole);
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected.front().reason == SettingRejectionReason::Invalid);
    CHECK(result.applied.empty());
  }
}

TEST_CASE("an accepted retention value is persisted for the next run")
{
  loadScratchConfig();
  REQUIRE(setRetention("7").rejected.empty());
  CHECK(ConfigService::getInt(kRetentionKey) == 7);
}

TEST_CASE("the purge with a shorter retention removes what is older and keeps what is newer")
{
  loadScratchConfig();
  REQUIRE(setRetention("7").rejected.empty());
  Database database;
  std::mutex mutex;
  const ConversationService service({.db = database.handle, .mutex = mutex, .policy = {.idleSeconds = 900}});
  recordAgo(service, 40, "de hace cuarenta dias");
  recordAgo(service, 5, "de hace cinco dias");

  CHECK(service.purgeExpired(kNow, ConfigService::getInt(kRetentionKey)) == 1);
  const auto tail = service.contextTail(kUser, 10);
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().content == "de hace cinco dias");
}

TEST_CASE("the purge with a longer retention keeps a turn that a shorter one would have removed")
{
  loadScratchConfig();
  REQUIRE(setRetention("60").rejected.empty());
  Database database;
  std::mutex mutex;
  const ConversationService service({.db = database.handle, .mutex = mutex, .policy = {.idleSeconds = 900}});
  recordAgo(service, 40, "de hace cuarenta dias");
  recordAgo(service, 5, "de hace cinco dias");

  CHECK(service.purgeExpired(kNow, ConfigService::getInt(kRetentionKey)) == 0);
  CHECK(service.contextTail(kUser, 10).size() == 2);
}
