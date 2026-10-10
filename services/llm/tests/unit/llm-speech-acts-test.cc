#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-render.hxx>

#include <text/iso-time.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <json/json.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

#include "support/require-value.hxx"

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

std::string tailOf(const Act& act, std::string_view lang)
{
  return actTail({.speech = Speech{.acts = {act}, .lang = std::string(lang), .now = kNow}, .contextBlock = {}});
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

TEST_CASE("every act renders a situation in both languages, without a command or an identifier")
{
  for (const Act& act : everyAct()) {
    for (const char* lang : {"es", "en"}) {
      const std::string tail = tailOf(act, lang);
      CHECK_FALSE(tail.empty());
      CHECK(tail.find(" -> ") == std::string::npos);
      CHECK(tail.find('{') == std::string::npos);
      CHECK(tail.find('}') == std::string::npos);
      CHECK(tail.find("\"kind\"") == std::string::npos);
      CHECK_FALSE(hasIdentifierToken(tail));
      CHECK(tail.find("ask_slot") == std::string::npos);
      CHECK(tail.find("calendar.") == std::string::npos);
      CHECK(tail.find("_") == std::string::npos);
      CHECK(tail.find("Formula UNA") == std::string::npos);
      CHECK(tail.find("Ask ONE") == std::string::npos);
    }
    CHECK(tailOf(act, "es") != tailOf(act, "en"));
  }
}

TEST_CASE("the ask situation is the missing fact plus the one thing to do")
{
  CHECK(tailOf(askSlot(), "es") ==
        "Nota de la app: falta la fecha y hora; pregunta solo por eso en una sola pregunta corta.");
  CHECK(tailOf(askSlot(), "en") == "App note: missing the date and time; ask for it in one short question.");
  const Act title = AskSlot{.slot = "title",
                            .tool = "task.create",
                            .knownArgs = Json::Value(Json::objectValue),
                            .reason = AskReason::Missing,
                            .dates = {},
                            .options = {}};
  CHECK(tailOf(title, "es") ==
        "Nota de la app: falta el nombre de la tarea; pregunta solo por eso en una sola pregunta corta.");
}

TEST_CASE("the slot is rendered as a human label, never as its name or its tool")
{
  CHECK(tailOf(askSlot(), "es").find("la fecha y hora") != std::string::npos);
  CHECK(tailOf(askSlot(), "en").find("the date and time") != std::string::npos);
  CHECK(tailOf(askSlot(), "es").find("starts_at") == std::string::npos);
  CHECK(tailOf(askSlot(), "en").find("calendar.create_event") == std::string::npos);
}

TEST_CASE("the situation comes first and the context block last, with no trailing system command")
{
  const Speech speech{.acts = {askSlot()}, .lang = "es", .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = "Contexto de la app"});
  const std::size_t situation = tail.find("Nota de la app:");
  const std::size_t context = tail.find("Contexto de la app");
  REQUIRE(situation != std::string::npos);
  REQUIRE(context != std::string::npos);
  CHECK(situation < context);
  CHECK(tail.find("\"kind\"") == std::string::npos);

  CHECK(actTail({.speech = Speech{.acts = {}, .lang = "es", .now = kNow}, .contextBlock = {}}).empty());
}

TEST_CASE("each act renders its own fields and nothing of another act's")
{
  CHECK(tailOf(askSlot(), "es").find("la fecha y hora") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("Cena con Marta") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("agendar el evento") != std::string::npos);
  CHECK(tailOf(confirm(), "es").find("falta la confirmación del usuario") != std::string::npos);
  CHECK(tailOf(choose(), "es").find("una tarea") != std::string::npos);
  CHECK(tailOf(choose(), "es").find("un recordatorio") != std::string::npos);
  CHECK(tailOf(done(), "es").find("Agendé la cena.") != std::string::npos);
  CHECK(tailOf(refused(), "es").find("no puedo crear proyectos") != std::string::npos);
  CHECK(tailOf(refused(), "es").find("anotar la tarea") != std::string::npos);
  CHECK(tailOf(offer(), "es").find("Productividad") != std::string::npos);
  CHECK(tailOf(unactionable(), "es").find("acción de Argus") != std::string::npos);
  CHECK(tailOf(declined(), "es").find("el usuario dijo que no.") != std::string::npos);
  CHECK(tailOf(misunderstood(), "en").find("the user was not understood.") != std::string::npos);
  CHECK(tailOf(done(), "es").find("el nombre de la tarea") == std::string::npos);
  CHECK(tailOf(choose(), "es").find("Agendé") == std::string::npos);
}

TEST_CASE("a note renders the tool's own read-back in place of a raw stamp, and never rewrites a fact it cannot improve")
{
  const auto hasIso = [](const std::string& text) {
    for (std::size_t at = 0; at + 16 <= text.size(); ++at) {
      bool ok = text[at + 4] == '-' && text[at + 7] == '-' && text[at + 10] == 'T' && text[at + 13] == ':';
      for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U})
        ok = ok && std::isdigit(static_cast<unsigned char>(text[at + index])) != 0;
      if (ok)
        return true;
    }
    return false;
  };
  const Done spoken{.tool = "calendar.create_event",
                     .fact = "Quedó agendado «Reunión con Andrea» para el jueves 15 a las cinco de la tarde.",
                     .readback = "el jueves 15 a las cinco de la tarde"};
  CHECK_FALSE(hasIso(tailOf(spoken, "es")));
  CHECK(tailOf(spoken, "es").find("jueves 15") != std::string::npos);
  const Done stamped{.tool = "calendar.create_event",
                      .fact = "Agendé «Reunión con Andrea» para 2026-10-15T17:00:00-05:00.",
                      .readback = "el jueves 15 a las cinco de la tarde"};
  CHECK_FALSE(hasIso(tailOf(stamped, "es")));
  CHECK(tailOf(stamped, "es").find("jueves 15") != std::string::npos);
  const Done unstamped{.tool = "calendar.create_event",
                       .fact = "Agendé «Reunión con Andrea» para 2026-10-15T17:00:00-05:00.",
                       .readback = {}};
  CHECK(tailOf(unstamped, "es") == "Nota de la app: Agendé «Reunión con Andrea» para 2026-10-15T17:00:00-05:00.");
}

TEST_CASE("an ask act that already knows a value does not repeat it back")
{
  Json::Value known(Json::objectValue);
  known["title"] = "Cena";
  const Act ask = AskSlot{.slot = "starts_at",
                          .tool = "calendar.create_event",
                          .knownArgs = std::move(known),
                          .reason = AskReason::Missing,
                          .dates = {},
                          .options = {}};
  const std::string tail = tailOf(ask, "es");
  CHECK(tail.find("Cena") == std::string::npos);
  CHECK(tail.find("el nombre de la tarea") == std::string::npos);
  CHECK(tail.find("ya tienes") == std::string::npos);
  CHECK(tail.find("\"title\"") == std::string::npos);
}

TEST_CASE("a date part is carried as data and resolved against now when the situation is built")
{
  const Speech speech{.acts = {AskSlot{.slot = "starts_at",
                                       .tool = "calendar.create_event",
                                       .knownArgs = Json::Value(Json::objectValue),
                                       .reason = AskReason::DatePassed,
                                       .dates = {{.kind = DateKind::Clock, .epoch = kNow}},
                                       .options = {}}},
                      .lang = "es",
                      .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = {}});
  CHECK(tail.find(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "es", kNow)) != std::string::npos);
  CHECK(tail.find("esa hora ya pasó hoy") != std::string::npos);
  CHECK(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "es", kNow).starts_with("a l"));
  CHECK(dateSurface(DatePart{.kind = DateKind::Clock, .epoch = kNow}, "en", kNow).starts_with("at "));
  CHECK(dateSurface(DatePart{.kind = DateKind::Today, .epoch = kNow}, "es", kNow).starts_with("hoy "));
  const int64_t later = kNow + 10LL * 86400;
  CHECK(dateSurface(DatePart{.kind = DateKind::CalendarDate, .epoch = later}, "es", kNow).starts_with("el "));
  CHECK(dateSurface(DatePart{.kind = DateKind::CalendarDate, .epoch = later}, "en", kNow).starts_with("on "));
}

TEST_CASE("several acts of one turn render as several situation lines")
{
  const Speech speech{.acts = {Done{.tool = "project.create", .fact = "Proyecto creado.", .readback = {}},
                               Done{.tool = "task.create", .fact = "Tarea anotada.", .readback = {}}},
                      .lang = "en",
                      .now = kNow};
  const std::string tail = actTail({.speech = speech, .contextBlock = {}});
  CHECK(tail.find("Proyecto creado.") != std::string::npos);
  CHECK(tail.find("Tarea anotada.") != std::string::npos);
  CHECK(tail.find("App note: ") != std::string::npos);
  CHECK(tail.find('\n') != std::string::npos);
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

TEST_CASE("the examples are the comparison, not the main variant, and are off unless asked for")
{
  const Speech speech{.acts = {askSlot()}, .lang = "es", .now = kNow};
  const std::string without = actTail({.speech = speech, .contextBlock = {}});
  const std::string with = actTail({.speech = speech, .contextBlock = {}, .examples = true});
  CHECK(without.find(" -> ") == std::string::npos);
  REQUIRE(with.find(" -> ") != std::string::npos);
  const std::span<const ExampleLine> lines = exampleLines(askSlot(), "es");
  for (const ExampleLine& line : lines) {
    CHECK(with.find(line.first) != std::string::npos);
    CHECK(with.find(line.second) != std::string::npos);
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
    const std::span<const ExampleLine> lines = exampleLines(requireValue(found), lang);
    REQUIRE(index >= 0);
    REQUIRE(static_cast<std::size_t>(index) < lines.size());
    CHECK(lines[static_cast<std::size_t>(index)].first == node["user"].asString());
    CHECK(lines[static_cast<std::size_t>(index)].second == node["reply"].asString());
    ++rows;
  }
  CHECK(rows == 36);
}

TEST_CASE("every act's rendered prompt carries no fixed text of the other language")
{
  const char* esPath = std::getenv("ARGUS_TEST_CALL_PROMPT_ES");
  const char* enPath = std::getenv("ARGUS_TEST_CALL_PROMPT_EN");
  REQUIRE_MESSAGE(esPath != nullptr, "ARGUS_TEST_CALL_PROMPT_ES is not set");
  REQUIRE_MESSAGE(enPath != nullptr, "ARGUS_TEST_CALL_PROMPT_EN is not set");
  const auto readFile = [](const char* path) {
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };
  const std::string personaEs = readFile(esPath);
  const std::string personaEn = readFile(enPath);

  const auto wordsOf = [](std::string_view text) {
    const std::string folded = text_norm::folded(std::string(text));
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < folded.size()) {
      const std::size_t end = std::min(folded.find(' ', at), folded.size());
      if (end > at)
        words.push_back(folded.substr(at, end - at));
      at = end + 1;
    }
    return words;
  };
  const auto contains = [](const std::vector<std::string>& hay, const std::vector<std::string>& needle) {
    if (needle.empty() || needle.size() > hay.size())
      return false;
    for (std::size_t at = 0; at + needle.size() <= hay.size(); ++at) {
      bool hit = true;
      for (std::size_t index = 0; index < needle.size() && hit; ++index)
        hit = hay.at(at + index) == needle.at(index);
      if (hit)
        return true;
    }
    return false;
  };
  const auto lines = [](const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
      const std::size_t end = text.find('\n', at);
      const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
      if (!line.empty())
        out.push_back(line);
      if (end == std::string::npos)
        break;
      at = end + 1;
    }
    return out;
  };

  for (const Act& act : everyAct()) {
    for (const char* lang : {"es", "en"}) {
      const bool english = std::string_view(lang) == "en";
      const std::string& persona = english ? personaEn : personaEs;
      const std::string_view other = english ? "es" : "en";
      const std::string prompt = persona + "\n" + tailOf(act, lang);
      const std::vector<std::string> promptWords = wordsOf(prompt);

      std::vector<std::string> foreign = lines(english ? personaEs : personaEn);
      for (const Act& sample : everyAct())
        foreign.emplace_back(tailOf(sample, other));
      std::size_t leaked = 0;
      for (const std::string& phrase : foreign)
        if (contains(promptWords, wordsOf(phrase)))
          ++leaked;
      CHECK_MESSAGE(leaked == 0,
                    std::string(actName(act)) << " [" << lang << "] leaks " << leaked << " fixed phrases of " << other);

      const std::vector<std::string> own = lines(persona);
      REQUIRE_MESSAGE(!own.empty(), "the persona fixture for " << lang << " is empty");
      CHECK(contains(promptWords, wordsOf(own.front())));
    }
  }
}
