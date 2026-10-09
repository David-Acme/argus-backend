#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-render.hxx>

#include <text/iso-time.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
using namespace turn::speech;

constexpr int64_t kNow = 1791700000;

Act askSlot()
{
  return AskSlot{.slot = "starts_at",
                 .tool = "calendar.create_event",
                 .knownArgs = Json::Value(Json::objectValue),
                 .reason = AskReason::Missing,
                 .dates = {},
                 .options = {}};
}

Act confirm()
{
  Json::Value args(Json::objectValue);
  args["title"] = "Cena con Marta";
  return Confirm{.action = "calendar.create_event",
                 .args = std::move(args),
                 .irreversible = false,
                 .toolPreview = {},
                 .module = {}};
}

Act choose()
{
  return Choose{.options = {"task.create", "memory.remind"}};
}

Act done()
{
  return Done{.tool = "calendar.create_event", .fact = "Agendé la cena.", .readback = "el jueves 8 a las 5"};
}

Act refused()
{
  return Refused{.tool = "task.create", .reason = "project_create_unavailable"};
}

Act offer()
{
  return Offer{.module = "productivity", .name = "Productividad", .facts = "esta apagado", .pendingIntent = {}};
}

Act declined()
{
  return Declined{};
}

Act unactionable()
{
  return Unactionable{.reason = "no_matching_action"};
}

Act misunderstood()
{
  return Misunderstood{};
}

std::vector<Act> everyAct()
{
  return {askSlot(), confirm(), choose(), done(), refused(), offer(), declined(), unactionable(), misunderstood()};
}
}

TEST_CASE("every act has its own name and its own question shape")
{
  CHECK(actName(askSlot()) == "ask_slot");
  CHECK(actName(confirm()) == "confirm");
  CHECK(actName(choose()) == "choose");
  CHECK(actName(done()) == "done");
  CHECK(actName(refused()) == "refused");
  CHECK(actName(offer()) == "offer");
  CHECK(actName(declined()) == "declined");
  CHECK(actName(unactionable()) == "unactionable");
  CHECK(actName(misunderstood()) == "misunderstood");

  CHECK(isQuestion(askSlot()));
  CHECK(isQuestion(confirm()));
  CHECK(isQuestion(choose()));
  CHECK_FALSE(isQuestion(done()));
  CHECK_FALSE(isQuestion(refused()));
  CHECK_FALSE(isQuestion(offer()));
  CHECK_FALSE(isQuestion(declined()));
  CHECK_FALSE(isQuestion(unactionable()));
  CHECK_FALSE(isQuestion(misunderstood()));
}

TEST_CASE("only the slot act names a slot, and every reason has a name")
{
  CHECK(slotOf(askSlot()) == "starts_at");
  CHECK(slotOf(confirm()).empty());
  CHECK(slotOf(done()).empty());
  CHECK(reasonName(AskReason::Missing) == "missing");
  CHECK(reasonName(AskReason::AmbiguousDate) == "ambiguous_date");
  CHECK(reasonName(AskReason::DatePassed) == "date_passed");
  CHECK(reasonName(AskReason::BeyondRange) == "beyond_range");
  CHECK(reasonName(AskReason::ProjectChoice) == "project_choice");
  CHECK(reasonName(AskReason::ProjectName) == "project_name");
  CHECK(reasonName(AskReason::ProjectNoneYet) == "project_none_yet");
}

TEST_CASE("every act renders an instruction in both languages and never one for the other's act")
{
  for (const Act& act : everyAct()) {
    const std::string_view es = instructionLine(act, "es");
    const std::string_view en = instructionLine(act, "en");
    CHECK_FALSE(es.empty());
    CHECK_FALSE(en.empty());
    CHECK(es != en);
  }
  CHECK(instructionLine(askSlot(), "es").find("UNA pregunta") != std::string_view::npos);
  CHECK(instructionLine(askSlot(), "en").find("Ask ONE question") != std::string_view::npos);
  CHECK(instructionLine(confirm(), "es").find("si confirma") != std::string_view::npos);
  CHECK(instructionLine(done(), "en").find("what was done") != std::string_view::npos);
  CHECK(instructionLine(misunderstood(), "es").find("No lo has entendido") != std::string_view::npos);
}

TEST_CASE("the tail carries the instructions, one line of act JSON and the context block, in that order")
{
  const Speech speech{.acts = {askSlot()}, .lang = "es", .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = "Contexto de la app"});
  const std::size_t instruction = tail.find("Formula UNA pregunta");
  const std::size_t act = tail.find("\"kind\":\"ask_slot\"");
  const std::size_t context = tail.find("Contexto de la app");
  REQUIRE(instruction != std::string::npos);
  REQUIRE(act != std::string::npos);
  REQUIRE(context != std::string::npos);
  CHECK(instruction < act);
  CHECK(act < context);

  CHECK(actTail({.speech = Speech{.acts = {}, .lang = "es", .now = kNow}, .contextBlock = {}}).empty());
}

TEST_CASE("each act renders its own fields and nothing of another act's")
{
  const auto tailOf = [](const Act& act, std::string_view lang) {
    return actTail({.speech = Speech{.acts = {act}, .lang = std::string(lang), .now = kNow}, .contextBlock = {}});
  };
  CHECK(tailOf(askSlot(), "es").find("\"slot\":\"starts_at\"") != std::string::npos);
  CHECK(tailOf(askSlot(), "es").find("\"tool\":\"calendar.create_event\"") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("\"title\":\"Cena con Marta\"") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("\"irreversible\":false") != std::string::npos);
  CHECK(tailOf(choose(), "es").find("\"options\":[\"task.create\",\"memory.remind\"]") != std::string::npos);
  CHECK(tailOf(done(), "es").find("\"readback\":\"el jueves 8 a las 5\"") != std::string::npos);
  CHECK(tailOf(refused(), "es").find("\"reason\":\"project_create_unavailable\"") != std::string::npos);
  CHECK(tailOf(offer(), "es").find("\"name\":\"Productividad\"") != std::string::npos);
  CHECK(tailOf(unactionable(), "es").find("\"reason\":\"no_matching_action\"") != std::string::npos);
  CHECK(tailOf(declined(), "es").find("\"kind\":\"declined\"") != std::string::npos);
  CHECK(tailOf(misunderstood(), "en").find("\"kind\":\"misunderstood\"") != std::string::npos);
  CHECK(tailOf(done(), "es").find("\"slot\"") == std::string::npos);
  CHECK(tailOf(choose(), "es").find("\"readback\"") == std::string::npos);
}

TEST_CASE("a date part is carried as data and resolved against now when the tail is built")
{
  const Speech speech{.acts = {AskSlot{.slot = "starts_at",
                                       .tool = "calendar.create_event",
                                       .knownArgs = Json::Value(Json::objectValue),
                                       .reason = AskReason::DatePassed,
                                       .dates = {{DateKind::Clock, kNow}},
                                       .options = {}}},
                      .lang = "es",
                      .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = {}});
  CHECK(tail.find("\"kind\":\"clock\"") != std::string::npos);
  CHECK(tail.find(iso_time::format(kNow)) != std::string::npos);
  CHECK(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "es", kNow).starts_with("a l"));
  CHECK(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "en", kNow).starts_with("at "));
  CHECK(dateSurface(DatePart{.kind = DateKind::Today, .epoch = kNow}, "es", kNow).starts_with("hoy "));
  const int64_t later = kNow + 10 * 86400;
  CHECK(dateSurface(DatePart{.kind = DateKind::CalendarDate, .epoch = later}, "es", kNow).starts_with("el "));
  CHECK(dateSurface(DatePart{.kind = DateKind::CalendarDate, .epoch = later}, "en", kNow).starts_with("on "));
}

TEST_CASE("several acts of one turn render as several JSON lines under one instruction")
{
  const Speech speech{.acts = {Done{.tool = "project.create", .fact = "Proyecto creado.", .readback = {}},
                               Done{.tool = "task.create", .fact = "Tarea anotada.", .readback = {}}},
                      .lang = "en",
                      .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = {}});
  CHECK(tail.find("\"fact\":\"Proyecto creado.\"") != std::string::npos);
  CHECK(tail.find("\"fact\":\"Tarea anotada.\"") != std::string::npos);
  CHECK(tail.find("Tell the user") != std::string::npos);
}
