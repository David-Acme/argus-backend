#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-render.hxx>

#include <text/iso-time.hxx>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <json/json.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

bool identifierShaped(std::string_view token)
{
  const auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
  for (std::size_t at = 0; at < token.size(); ++at) {
    if (token[at] != '.' && token[at] != '_')
      continue;
    if (at == 0 || at + 1 >= token.size())
      continue;
    if (alnum(token[at - 1]) && alnum(token[at + 1]))
      return true;
  }
  return false;
}

bool hasIdentifierToken(std::string_view text)
{
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = std::min(text.find_first_of(" \n\t,:", at), text.size());
    if (end > at && identifierShaped(text.substr(at, end - at)))
      return true;
    at = end + 1;
  }
  return false;
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

TEST_CASE("the rendered prompt of every act carries no identifier-shaped token and never says missing value")
{
  for (const Act& act : everyAct())
    for (const char* lang : {"es", "en"}) {
      const std::string tail =
          actTail({.speech = Speech{.acts = {act}, .lang = lang, .now = kNow}, .contextBlock = {}});
      CHECK_FALSE(hasIdentifierToken(tail));
      std::string lowered = tail;
      for (char& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      CHECK(lowered.find("missing value") == std::string::npos);
      CHECK(tail.find("ask_slot") == std::string::npos);
      CHECK(tail.find("calendar.") == std::string::npos);
      CHECK(tail.find("_") == std::string::npos);
    }
}

TEST_CASE("the slot is rendered as a human label, never as its name or its tool")
{
  const auto tailOf = [](const Act& act, std::string_view lang) {
    return actTail({.speech = Speech{.acts = {act}, .lang = std::string(lang), .now = kNow}, .contextBlock = {}});
  };
  CHECK(tailOf(askSlot(), "es").find("la fecha y hora") != std::string::npos);
  CHECK(tailOf(askSlot(), "en").find("the date and time") != std::string::npos);
  CHECK(tailOf(askSlot(), "es").find("starts_at") == std::string::npos);
  CHECK(tailOf(askSlot(), "en").find("calendar.create_event") == std::string::npos);
}

TEST_CASE("the tail carries the instructions, one line of act JSON and the context block, in that order")
{
  const Speech speech{.acts = {askSlot()}, .lang = "es", .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = "Contexto de la app"});
  const std::size_t instruction = tail.find("Formula UNA pregunta");
  const std::size_t act = tail.find("\"kind\":\"pregunta\"");
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
  CHECK(tailOf(askSlot(), "es").find("\"about\":\"la fecha y hora\"") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("\"el nombre de la tarea\":\"Cena con Marta\"") != std::string::npos);
  CHECK(tailOf(choose(), "es").find("\"options\":[\"una tarea\",\"un recordatorio\"]") != std::string::npos);
  CHECK(tailOf(done(), "es").find("\"readback\":\"el jueves 8 a las 5\"") != std::string::npos);
  CHECK(tailOf(refused(), "es").find("\"because\":\"no puedo crear proyectos\"") != std::string::npos);
  CHECK(tailOf(offer(), "es").find("\"about\":\"Productividad\"") != std::string::npos);
  CHECK(tailOf(unactionable(), "es").find("\"kind\":\"sin acción\"") != std::string::npos);
  CHECK(tailOf(declined(), "es").find("\"kind\":\"el usuario dijo que no\"") != std::string::npos);
  CHECK(tailOf(misunderstood(), "en").find("\"kind\":\"not understood\"") != std::string::npos);
  CHECK(tailOf(done(), "es").find("\"about\"") == std::string::npos);
  CHECK(tailOf(choose(), "es").find("\"readback\"") == std::string::npos);
}

TEST_CASE("an ask act that already knows a value lists it by its human label")
{
  Json::Value known(Json::objectValue);
  known["title"] = "Cena";
  const Act ask = AskSlot{.slot = "starts_at",
                          .tool = "calendar.create_event",
                          .knownArgs = std::move(known),
                          .reason = AskReason::Missing,
                          .dates = {},
                          .options = {}};
  const std::string tail =
      actTail({.speech = Speech{.acts = {ask}, .lang = "es", .now = kNow}, .contextBlock = {}});
  CHECK(tail.find("\"known\":[\"el nombre de la tarea\"]") != std::string::npos);
  CHECK(tail.find("\"title\"") == std::string::npos);
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
  CHECK(tail.find(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "es", kNow)) != std::string::npos);
  CHECK(tail.find("\"because\":\"esa hora ya pasó hoy\"") != std::string::npos);
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

TEST_CASE("every act carries two few-shot examples per language, each free of an identifier")
{
  for (const Act& act : everyAct())
    for (const char* lang : {"es", "en"}) {
      const std::span<const ExampleLine> lines = exampleLines(act, lang);
      REQUIRE(lines.size() == 2);
      for (const ExampleLine& line : lines) {
        CHECK_FALSE(line.first.empty());
        CHECK_FALSE(line.second.empty());
        CHECK_FALSE(hasIdentifierToken(line.first));
        CHECK_FALSE(hasIdentifierToken(line.second));
      }
    }
}

TEST_CASE("the few-shot examples are pinned as a fixture and match it act by act")
{
  const char* path = std::getenv("ARGUS_TEST_SPEECH_EXAMPLES");
  REQUIRE_MESSAGE(path != nullptr, "ARGUS_TEST_SPEECH_EXAMPLES is not set");
  std::ifstream in(path);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  int rows = 0;
  std::string line;
  Json::CharReaderBuilder builder;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    Json::Value node;
    std::string errors;
    std::istringstream stream(line);
    const bool parsed = Json::parseFromStream(builder, stream, &node, &errors);
    REQUIRE_MESSAGE(parsed, "cannot parse " << line << ": " << errors);
    const std::string actName = node["act"].asString();
    const std::string lang = node["lang"].asString();
    const int index = node["index"].asInt();
    std::optional<Act> found;
    for (const Act& act : everyAct())
      if (std::string(turn::speech::actName(act)) == actName)
        found = act;
    REQUIRE_MESSAGE(found.has_value(), "no act named " << actName);
    const std::span<const ExampleLine> lines = exampleLines(*found, lang);
    REQUIRE(index >= 0);
    REQUIRE(static_cast<std::size_t>(index) < lines.size());
    CHECK(lines[static_cast<std::size_t>(index)].first == node["user"].asString());
    CHECK(lines[static_cast<std::size_t>(index)].second == node["reply"].asString());
    ++rows;
  }
  CHECK(rows == 36);
}
