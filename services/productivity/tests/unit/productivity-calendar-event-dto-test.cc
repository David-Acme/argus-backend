#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/validation-exception.hxx>
#include <feature/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/calendar-event/dtos/update-calendar-event-dto.hxx>
#include <shared/services/recurrence/recurrence.hxx>

#include <json/json.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace
{
constexpr int64_t kStartsAt = 1'790'000'000;

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

Json::Value createBody(const Json::Value& rule)
{
  Json::Value json;
  json["title"] = "Clase de yoga";
  json["startsAt"] = static_cast<Json::Int64>(kStartsAt);
  json["recurrenceRule"] = rule;
  return json;
}

Json::Value updateBody(const Json::Value& rule)
{
  Json::Value json(Json::objectValue);
  json["recurrenceRule"] = rule;
  return json;
}

std::optional<ValidationErrors> refusalOf(const Json::Value& body)
{
  try {
    static_cast<void>(CreateCalendarEventDto::fromJson(body));
  }
  catch (const ValidationException& refused) {
    return refused.errors();
  }
  return std::nullopt;
}
}

TEST_CASE("every rule the reminders' parser reads is a rule an event keeps")
{
  const Json::Value fixture = loadCases();
  REQUIRE(fixture["cases"].size() > 0);
  for (const auto& entry : fixture["cases"]) {
    const std::string rule = entry["rule"].asString();
    CAPTURE(rule);
    REQUIRE(parseRecurrenceRule(rule).has_value());
    CHECK(CreateCalendarEventDto::fromJson(createBody(rule)).recurrenceRule == rule);
    CHECK(UpdateCalendarEventDto::fromJson(updateBody(rule)).recurrenceRule == rule);
  }
}

TEST_CASE("every rule the reminders' parser refuses is a 422 for an event, created or edited")
{
  const Json::Value fixture = loadCases();
  REQUIRE(fixture["invalid"].size() > 1);
  for (const auto& entry : fixture["invalid"]) {
    const std::string rule = entry.asString();
    if (rule.empty())
      continue;
    CAPTURE(rule);
    CHECK_FALSE(parseRecurrenceRule(rule).has_value());
    CHECK_THROWS_AS(static_cast<void>(CreateCalendarEventDto::fromJson(createBody(rule))), ValidationException);
    CHECK_THROWS_AS(static_cast<void>(UpdateCalendarEventDto::fromJson(updateBody(rule))), ValidationException);
  }
}

TEST_CASE("the refusal names the field and says what a rule may be")
{
  const auto refusal = refusalOf(createBody("FREQ=WEEKLY;BYDAY=MO,WE"));
  REQUIRE(refusal.has_value());
  REQUIRE(refusal->contains("recurrenceRule"));
  const auto& messages = refusal->at("recurrenceRule");
  REQUIRE(messages.size() == 1);
  CHECK(messages.front() == kRecurrenceRuleProblem);
}

TEST_CASE("an event without a rule, or with an empty one, is accepted as before")
{
  Json::Value withoutRule = createBody(Json::Value());
  withoutRule.removeMember("recurrenceRule");
  CHECK_FALSE(CreateCalendarEventDto::fromJson(withoutRule).recurrenceRule.has_value());
  CHECK(CreateCalendarEventDto::fromJson(createBody("")).recurrenceRule == std::string{});
  CHECK(UpdateCalendarEventDto::fromJson(updateBody("")).recurrenceRule == std::string{});
  CHECK_FALSE(UpdateCalendarEventDto::fromJson(Json::Value(Json::objectValue)).recurrenceRule.has_value());
  CHECK_FALSE(refusalOf(createBody("FREQ=DAILY")).has_value());
}
