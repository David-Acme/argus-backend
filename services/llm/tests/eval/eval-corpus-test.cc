#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "eval-case.hxx"
#include "eval-report.hxx"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

const std::vector<eval::EvalCase>& corpus()
{
  static const eval::LoadedCases loaded = eval::loadCases(ARGUS_EVAL_CASES);
  REQUIRE_MESSAGE(loaded.error.empty(), loaded.error);
  return loaded.cases;
}

bool contains(const std::vector<std::string>& values, const std::string& wanted)
{
  return std::ranges::find(values, wanted) != values.end();
}

}

TEST_CASE("every judge case is well formed and unique")
{
  const std::set<std::string> routes{"memory_save", "memory_recall", "reminder_set", "memory_forget", "camera", "none"};
  const std::set<std::string> variants{"neutral", "pe", "stt", "en"};
  const std::set<std::string> roles{"owner", "resident", "guard", "guest"};
  std::set<std::string> ids;
  CHECK(corpus().size() >= 700);
  for (const auto& item : corpus()) {
    CHECK_MESSAGE(ids.insert(item.id).second, "duplicate id " << item.id);
    CHECK_MESSAGE(routes.contains(item.route), item.id << " route " << item.route);
    CHECK_MESSAGE(variants.contains(item.variant), item.id << " variant " << item.variant);
    CHECK_MESSAGE(roles.contains(item.role), item.id << " role " << item.role);
    CHECK_MESSAGE((item.lang == "es" || item.lang == "en"), item.id << " lang " << item.lang);
    CHECK_MESSAGE(!item.script.empty(), item.id << " has no utterance");
    const int judged = (item.calls.empty() ? 0 : 1) + (item.inactive ? 1 : 0) + (item.confirm ? 1 : 0) +
                       (item.offerAccept ? 1 : 0) + (item.offerDecline ? 1 : 0);
    CHECK_MESSAGE(judged <= 1, item.id << " carries " << judged << " kinds of expectation");
  }
}

TEST_CASE("agenda, calendar, task and project utterances are labelled none in every variant")
{
  std::map<std::string, int> perVariant;
  for (const auto& item : corpus()) {
    if (item.group != "productivity")
      continue;
    CHECK_MESSAGE(item.route == "none", item.id << " routes to " << item.route);
    ++perVariant[item.variant];
  }
  CHECK(perVariant["neutral"] >= 30);
  CHECK(perVariant["pe"] >= 10);
  CHECK(perVariant["stt"] >= 8);
  CHECK(perVariant["en"] >= 20);
}

TEST_CASE("every productivity request is asked again with the module off")
{
  std::map<std::string, const eval::EvalCase*> byId;
  for (const auto& item : corpus())
    byId[item.id] = &item;
  int owners = 0;
  int members = 0;
  for (const auto& item : corpus()) {
    if (item.group != "productivity" || item.calls.empty())
      continue;
    const auto twin = std::ranges::find_if(corpus(), [&item](const eval::EvalCase& other) {
      return other.twin == item.id && other.role == "owner";
    });
    REQUIRE_MESSAGE(twin != corpus().end(), item.id << " has no owner twin with productivity off");
    CHECK_FALSE(contains(twin->modules, "productivity"));
    CHECK(twin->utterance() == item.utterance());
    if (!twin->inactive) {
      FAIL_CHECK(item.id << " twin does not expect the module-off answer");
      continue;
    }
    CHECK(twin->inactive->module == "productivity");
    CHECK(twin->inactive->attempted == item.calls.front().tool);
    CHECK(twin->inactive->audience == "owner");
    CHECK(twin->calls.empty());
    ++owners;
  }
  for (const auto& item : corpus()) {
    if (!item.inactive || item.role == "owner")
      continue;
    CHECK(item.inactive.value().audience == "member");
    ++members;
  }
  CHECK(owners >= 90);
  CHECK(members >= 20);
}

TEST_CASE("a destructive tool is judged both before and after the spoken yes")
{
  std::map<std::string, std::set<std::string>> phases;
  for (const auto& item : corpus()) {
    if (!item.confirm)
      continue;
    phases[item.confirm.value().tool].insert(item.confirm.value().phase);
    if (item.confirm.value().phase == "execute")
      CHECK_MESSAGE(item.script.size() == 2, item.id << " needs the request and the yes");
  }
  for (const char* tool : {"calendar.cancel_event", "modules.disable"}) {
    CHECK(phases[tool].contains("ask"));
    CHECK(phases[tool].contains("execute"));
  }
}

TEST_CASE("the Wilson bounds and the gates behave as documented")
{
  const eval::Interval half = eval::wilson({.hits = 5, .total = 10});
  CHECK(half.lower == doctest::Approx(0.2366).epsilon(0.01));
  CHECK(half.upper == doctest::Approx(0.7634).epsilon(0.01));
  CHECK(eval::wilson({.hits = 0, .total = 0}).upper == 0.0);

  const std::vector<eval::Gate> gates{{.metric = "a", .min = 0.9, .max = std::nullopt},
                                      {.metric = "b", .min = std::nullopt, .max = 0.1},
                                      {.metric = "missing", .min = 0.0, .max = std::nullopt}};
  const eval::Verdict verdict = eval::check(gates, {{"a", 0.8}, {"b", 0.2}});
  CHECK(verdict.failures.size() == 3);
  CHECK(eval::check(gates, {{"a", 0.9}, {"b", 0.1}, {"missing", 1.0}}).passed());
}
