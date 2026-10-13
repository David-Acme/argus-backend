#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/recurrence/recurrence.hxx>

#include <json/json.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{
Json::Value loadCases()
{
  std::ifstream file(ARGUS_RECURRENCE_CASES);
  std::stringstream text;
  text << file.rdbuf();
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  std::istringstream stream(text.str());
  Json::parseFromStream(builder, stream, &root, &errors);
  return root;
}

void useZone(const std::string& zone)
{
  setenv("TZ", zone.c_str(), 1);
  tzset();
}

int64_t epochOfWall(const std::string& wall)
{
  std::tm local{};
  std::istringstream stream(wall);
  stream >> std::get_time(&local, "%Y-%m-%dT%H:%M:%S");
  local.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&local));
}

std::string wallOfEpoch(int64_t epoch)
{
  const auto moment = static_cast<std::time_t>(epoch);
  std::tm local{};
  localtime_r(&moment, &local);
  std::ostringstream text;
  text << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
  return text.str();
}

std::vector<std::string> stringsOf(const Json::Value& array)
{
  std::vector<std::string> values;
  for (const auto& item : array)
    values.push_back(item.asString());
  return values;
}
}

TEST_CASE("the shared fixtures hold in the zone each case names")
{
  const Json::Value root = loadCases();
  REQUIRE(root["cases"].isArray());
  for (const auto& item : root["cases"]) {
    INFO(item["name"].asString());
    useZone(item["zone"].asString());
    const auto rule = parseRecurrenceRule(item["rule"].asString());
    REQUIRE(rule.has_value());
    const int64_t anchor = epochOfWall(item["anchor"].asString());
    if (item.isMember("expect")) {
      std::vector<int64_t> epochs;
      for (int index = 0; index < item["count"].asInt(); ++index) {
        const auto occurrence = occurrenceAt({.rule = *rule, .anchor = anchor, .index = index});
        REQUIRE(occurrence.has_value());
        epochs.push_back(*occurrence);
      }
      std::vector<std::string> walls;
      walls.reserve(epochs.size());
      for (const int64_t epoch : epochs)
        walls.push_back(wallOfEpoch(epoch));
      CHECK(walls == stringsOf(item["expect"]));
      if (item.isMember("displayZone")) {
        useZone(item["displayZone"].asString());
        std::vector<std::string> displayed;
        displayed.reserve(epochs.size());
        for (const int64_t epoch : epochs)
          displayed.push_back(wallOfEpoch(epoch));
        CHECK(displayed == stringsOf(item["expectDisplay"]));
      }
    }
    if (item.isMember("after")) {
      useZone(item["zone"].asString());
      const auto next = firstOccurrenceAfter(
          {.rule = *rule, .anchor = anchor, .moment = epochOfWall(item["after"].asString())});
      REQUIRE(next.has_value());
      CHECK(wallOfEpoch(*next) == item["expectFirst"].asString());
    }
  }
}

TEST_CASE("the shared invalid rules are refused")
{
  const Json::Value root = loadCases();
  for (const auto& text : root["invalid"]) {
    INFO(text.asString());
    CHECK_FALSE(parseRecurrenceRule(text.asString()).has_value());
  }
}

TEST_CASE("a valid rule keeps its frequency and interval")
{
  const auto rule = parseRecurrenceRule("FREQ=WEEKLY;INTERVAL=2");
  REQUIRE(rule.has_value());
  const RecurrenceRule parsed = rule.value_or(RecurrenceRule{});
  CHECK(parsed.frequency == RecurrenceFrequency::Weekly);
  CHECK(parsed.interval == 2);
}

TEST_CASE("tomorrow keeps the wall time across a change of day length")
{
  useZone("America/New_York");
  const auto tomorrow = sameTimeTomorrow(epochOfWall("2026-03-07T08:00:00"));
  REQUIRE(tomorrow.has_value());
  CHECK(wallOfEpoch(tomorrow.value_or(0)) == "2026-03-08T08:00:00");
}
