#include <doctest/doctest.h>
#include <test-support/value-of.hxx>

#include <shared/services/recurrence/occurrence-exceptions.hxx>
#include <shared/services/recurrence/recurrence.hxx>
#include <shared/services/recurrence/series-window.hxx>

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

TEST_CASE("a monthly rule may name its day of the month")
{
  const auto rule = parseRecurrenceRule("FREQ=MONTHLY;INTERVAL=3;BYMONTHDAY=31");
  REQUIRE(rule.has_value());
  const RecurrenceRule parsed = rule.value_or(RecurrenceRule{});
  CHECK(parsed.frequency == RecurrenceFrequency::Monthly);
  CHECK(parsed.interval == 3);
  CHECK(parsed.monthDay.value_or(0) == 31);
  CHECK_FALSE(parseRecurrenceRule("FREQ=DAILY").value_or(RecurrenceRule{}).monthDay.has_value());
}

TEST_CASE("a series on a day of the month starts on its first such day")
{
  useZone("America/Lima");
  const auto rule = parseRecurrenceRule("FREQ=MONTHLY;BYMONTHDAY=31").value_or(RecurrenceRule{});
  CHECK(wallOfEpoch(seriesStart(rule, epochOfWall("2026-11-02T09:00:00"))) == "2026-11-30T09:00:00");
  CHECK(wallOfEpoch(seriesStart(rule, epochOfWall("2026-12-31T09:00:00"))) == "2026-12-31T09:00:00");
  const auto plain = parseRecurrenceRule("FREQ=MONTHLY").value_or(RecurrenceRule{});
  CHECK(seriesStart(plain, epochOfWall("2026-11-02T09:00:00")) == epochOfWall("2026-11-02T09:00:00"));
}

TEST_CASE("tomorrow keeps the wall time across a change of day length")
{
  useZone("America/New_York");
  const auto tomorrow = sameTimeTomorrow(epochOfWall("2026-03-07T08:00:00"));
  REQUIRE(tomorrow.has_value());
  CHECK(wallOfEpoch(tomorrow.value_or(0)) == "2026-03-08T08:00:00");
}

TEST_CASE("occurrence exceptions survive a round trip and drop what cannot be read")
{
  const auto parsed = parseOccurrenceExceptions(
      R"([{"occurrence":200,"cancelled":true,"title":"ignored"},)"
      R"({"occurrence":100,"startsAt":150,"endsAt":160,"title":"Later","location":"Porch"},)"
      R"({"occurrence":300},{"occurrence":"x"},7,{"occurrence":100,"cancelled":true}])");
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[0].occurrence == 100);
  CHECK(parsed[0].cancelled);
  CHECK_FALSE(parsed[0].startsAt.has_value());
  CHECK(parsed[1].occurrence == 200);
  CHECK(parsed[1].cancelled);
  CHECK_FALSE(parsed[1].title.has_value());

  const OccurrenceExceptions moved{{.occurrence = 100,
                                    .cancelled = false,
                                    .startsAt = 150,
                                    .endsAt = 160,
                                    .title = "Later",
                                    .location = "Porch"}};
  const auto again = parseOccurrenceExceptions(serializeOccurrenceExceptions(moved));
  REQUIRE(again.size() == 1);
  CHECK(again[0].startsAt.value_or(0) == 150);
  CHECK(again[0].endsAt.value_or(0) == 160);
  CHECK(again[0].title.value_or("") == "Later");
  CHECK(again[0].location.value_or("") == "Porch");

  CHECK(parseOccurrenceExceptions("not json").empty());
  CHECK(parseOccurrenceExceptions("{}").empty());
  CHECK(serializeOccurrenceExceptions({}) == "[]");
}

TEST_CASE("an exception replaces the one of its occurrence and a plain one restores it")
{
  OccurrenceExceptions exceptions;
  exceptions = withException(exceptions, {.occurrence = 300,
                                          .cancelled = true,
                                          .startsAt = std::nullopt,
                                          .endsAt = std::nullopt,
                                          .title = std::nullopt,
                                          .location = std::nullopt});
  exceptions = withException(exceptions, {.occurrence = 100,
                                          .cancelled = false,
                                          .startsAt = 120,
                                          .endsAt = std::nullopt,
                                          .title = std::nullopt,
                                          .location = std::nullopt});
  REQUIRE(exceptions.size() == 2);
  CHECK(exceptions.front().occurrence == 100);
  CHECK(scheduledOccurrence(exceptions, 120) == 100);
  CHECK(scheduledOccurrence(exceptions, 300) == 300);

  exceptions = withException(exceptions, {.occurrence = 100,
                                          .cancelled = false,
                                          .startsAt = std::nullopt,
                                          .endsAt = std::nullopt,
                                          .title = std::nullopt,
                                          .location = std::nullopt});
  REQUIRE(exceptions.size() == 1);
  CHECK(exceptions.front().occurrence == 300);
  CHECK(exceptionsFrom(exceptions, 301).empty());
  CHECK(exceptionsFrom(exceptions, 300).size() == 1);
}

TEST_CASE("an occurrence is found by its index and knows its neighbours")
{
  useZone("America/Lima");
  const auto rule = parseRecurrenceRule("FREQ=WEEKLY").value_or(RecurrenceRule{});
  const int64_t anchor = epochOfWall("2026-10-05T08:00:00");
  const int64_t second = epochOfWall("2026-10-12T08:00:00");
  const auto slot = occurrenceSlot({.rule = rule, .anchor = anchor, .moment = second});
  REQUIRE(slot.has_value());
  CHECK(slot.value_or(OccurrenceSlot{}).index == 1);
  CHECK(slot.value_or(OccurrenceSlot{}).previous.value_or(0) == anchor);
  CHECK(wallOfEpoch(slot.value_or(OccurrenceSlot{}).next.value_or(0)) == "2026-10-19T08:00:00");
  CHECK(occurrenceIndex({.rule = rule, .anchor = anchor, .moment = anchor}).value_or(-1) == 0);
  CHECK_FALSE(occurrenceIndex({.rule = rule, .anchor = anchor, .moment = second + 60}).has_value());
  CHECK_FALSE(occurrenceIndex({.rule = rule, .anchor = anchor, .moment = anchor - 604800}).has_value());
  CHECK_FALSE(occurrenceSlot({.rule = rule, .anchor = anchor, .moment = anchor})
                  .value_or(OccurrenceSlot{})
                  .previous.has_value());
}

TEST_CASE("the next firing skips cancelled occurrences and fires moved ones at their new time")
{
  useZone("America/Lima");
  const auto rule = parseRecurrenceRule("FREQ=DAILY").value_or(RecurrenceRule{});
  const int64_t anchor = epochOfWall("2026-10-05T08:00:00");
  const int64_t day1 = epochOfWall("2026-10-06T08:00:00");
  const int64_t day2 = epochOfWall("2026-10-07T08:00:00");
  const int64_t day3 = epochOfWall("2026-10-08T08:00:00");
  const OccurrenceExceptions exceptions{
      {.occurrence = day1,
       .cancelled = true,
       .startsAt = std::nullopt,
       .endsAt = std::nullopt,
       .title = std::nullopt,
       .location = std::nullopt},
      {.occurrence = day2,
       .cancelled = false,
       .startsAt = day2 + 7200,
       .endsAt = std::nullopt,
       .title = std::nullopt,
       .location = std::nullopt}};

  const auto skipped = nextFiring(
      {.rule = rule, .anchor = anchor, .after = anchor, .moment = anchor, .exceptions = exceptions});
  CHECK(skipped.value_or(NextFiring{}).occurrence == day2);
  CHECK(skipped.value_or(NextFiring{}).at == day2 + 7200);

  const auto behind = nextFiring(
      {.rule = rule, .anchor = anchor, .after = day1, .moment = day2 + 60, .exceptions = exceptions});
  CHECK(behind.value_or(NextFiring{}).occurrence == day2);
  CHECK(behind.value_or(NextFiring{}).at == day2 + 7200);

  const auto afterMoved = nextFiring(
      {.rule = rule, .anchor = anchor, .after = day2, .moment = day2 + 7200, .exceptions = exceptions});
  CHECK(afterMoved.value_or(NextFiring{}).occurrence == day3);
  CHECK(afterMoved.value_or(NextFiring{}).at == day3);

  const OccurrenceExceptions none;
  const auto plain = nextFiring(
      {.rule = rule, .anchor = anchor, .after = anchor, .moment = anchor, .exceptions = none});
  CHECK(plain.value_or(NextFiring{}).at == day1);
}

namespace
{
std::vector<std::string> startsOf(const std::vector<SeriesOccurrence>& occurrences)
{
  std::vector<std::string> walls;
  walls.reserve(occurrences.size());
  for (const auto& occurrence : occurrences)
    walls.push_back(wallOfEpoch(occurrence.startsAt));
  return walls;
}
}

TEST_CASE("a window of a series leaves skipped occurrences out and shows moved ones at their new time")
{
  useZone("America/Lima");
  const int64_t anchor = epochOfWall("2026-10-05T08:00:00");
  const int64_t second = epochOfWall("2026-10-06T08:00:00");
  const int64_t third = epochOfWall("2026-10-07T08:00:00");
  const int64_t fifth = epochOfWall("2026-10-09T08:00:00");
  const std::string exceptions = serializeOccurrenceExceptions(
      {{.occurrence = second,
        .cancelled = true,
        .startsAt = std::nullopt,
        .endsAt = std::nullopt,
        .title = std::nullopt,
        .location = std::nullopt},
       {.occurrence = third,
        .cancelled = false,
        .startsAt = third + 5400,
        .endsAt = std::nullopt,
        .title = std::string("Yoga en el parque"),
        .location = std::string("Parque")},
       {.occurrence = fifth,
        .cancelled = false,
        .startsAt = epochOfWall("2026-10-12T19:00:00"),
        .endsAt = std::nullopt,
        .title = std::nullopt,
        .location = std::nullopt}});
  const std::string rule = "FREQ=DAILY";
  const auto week = occurrencesWithin({.rule = rule,
                                       .startsAt = anchor,
                                       .endsAt = anchor + 3600,
                                       .exceptions = exceptions,
                                       .from = epochOfWall("2026-10-06T00:00:00"),
                                       .to = epochOfWall("2026-10-09T23:59:59")});
  CHECK(startsOf(week) == std::vector<std::string>{"2026-10-07T09:30:00", "2026-10-08T08:00:00"});
  REQUIRE(week.size() == 2);
  CHECK(week.front().occurrence == third);
  CHECK(week.front().endsAt.value_or(0) == third + 5400 + 3600);
  CHECK(week.front().title.value_or("") == "Yoga en el parque");
  CHECK(week.front().location.value_or("") == "Parque");
  CHECK(week.front().repeats);

  const auto movedIn = occurrencesWithin({.rule = rule,
                                          .startsAt = anchor,
                                          .endsAt = std::nullopt,
                                          .exceptions = exceptions,
                                          .from = epochOfWall("2026-10-12T12:00:00"),
                                          .to = epochOfWall("2026-10-12T23:59:59")});
  CHECK(startsOf(movedIn) == std::vector<std::string>{"2026-10-12T19:00:00"});
  REQUIRE(movedIn.size() == 1);
  CHECK(movedIn.front().occurrence == fifth);
}

TEST_CASE("a window of a row that does not repeat holds its own start when it falls inside")
{
  useZone("America/Lima");
  const int64_t start = epochOfWall("2026-10-05T08:00:00");
  const auto single = occurrencesWithin({.rule = std::nullopt,
                                         .startsAt = start,
                                         .endsAt = std::nullopt,
                                         .exceptions = "[]",
                                         .from = start - 60,
                                         .to = start + 60});
  REQUIRE(single.size() == 1);
  CHECK_FALSE(single.front().repeats);
  const std::string unreadable = "FREQ=HOURLY";
  CHECK(occurrencesWithin({.rule = unreadable,
                           .startsAt = start,
                           .endsAt = std::nullopt,
                           .exceptions = "[]",
                           .from = start + 1,
                           .to = start + 86400})
            .empty());
  const std::string monthly = "FREQ=MONTHLY;BYMONTHDAY=31";
  const auto months = occurrencesWithin({.rule = monthly,
                                         .startsAt = epochOfWall("2026-10-31T09:00:00"),
                                         .endsAt = std::nullopt,
                                         .exceptions = "[]",
                                         .from = epochOfWall("2027-02-01T00:00:00"),
                                         .to = epochOfWall("2027-04-30T23:59:59")});
  CHECK(startsOf(months) ==
        std::vector<std::string>{"2027-02-28T09:00:00", "2027-03-31T09:00:00", "2027-04-30T09:00:00"});
}

TEST_CASE("a snooze moves only the current occurrence and folds into the next when it reaches it")
{
  useZone("America/Lima");
  const auto rule = parseRecurrenceRule("FREQ=DAILY").value_or(RecurrenceRule{});
  const int64_t anchor = epochOfWall("2026-10-05T08:00:00");
  const int64_t second = epochOfWall("2026-10-06T08:00:00");
  const OccurrenceExceptions none;

  const auto moved = valueOf(snoozeSeries(
      {.rule = rule, .anchor = anchor, .scheduledAt = anchor, .exceptions = none, .target = anchor + 1800}));
  CHECK_FALSE(moved.folded);
  CHECK(moved.scheduledAt == anchor + 1800);
  REQUIRE(moved.exceptions.size() == 1);
  CHECK(moved.exceptions.front().occurrence == anchor);
  CHECK(moved.exceptions.front().startsAt.value_or(0) == anchor + 1800);

  const auto again = valueOf(snoozeSeries({.rule = rule,
                                           .anchor = anchor,
                                           .scheduledAt = anchor + 1800,
                                           .exceptions = moved.exceptions,
                                           .target = anchor + 3600}));
  REQUIRE(again.exceptions.size() == 1);
  CHECK(again.exceptions.front().occurrence == anchor);
  CHECK(again.exceptions.front().startsAt.value_or(0) == anchor + 3600);

  const auto folded = valueOf(snoozeSeries(
      {.rule = rule, .anchor = anchor, .scheduledAt = anchor, .exceptions = none, .target = second + 60}));
  CHECK(folded.folded);
  CHECK(folded.scheduledAt == second);
  REQUIRE(folded.exceptions.size() == 1);
  CHECK(folded.exceptions.front().cancelled);

  CHECK_FALSE(snoozeSeries(
                  {.rule = rule, .anchor = anchor, .scheduledAt = anchor + 60, .exceptions = none, .target = anchor + 600})
                  .has_value());
}
