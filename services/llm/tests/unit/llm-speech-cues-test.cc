#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-cues.hxx>
#include <feature/llm/services/turn/speech-guard.hxx>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace turn::speech;

constexpr int64_t kNow = 1791700000;

GuardVerdict asks(std::string_view slot, std::string lang, std::string_view reply,
                  Json::Value known = Json::Value(Json::objectValue))
{
  const Speech speech{.acts = {AskSlot{.slot = std::string(slot),
                                       .tool = "calendar.create_event",
                                       .knownArgs = std::move(known),
                                       .reason = AskReason::Missing,
                                       .dates = {},
                                       .options = {}}},
                      .lang = std::move(lang),
                      .now = kNow};
  return check({.speech = speech, .reply = reply, .wrote = false, .asked = true, .callsConfirmed = false});
}

GuardVerdict chooseOf(std::string lang, std::string_view reply)
{
  const Speech speech{.acts = {Choose{.options = {"calendar.create_event", "memory.forget"}}},
                      .lang = std::move(lang),
                      .now = kNow};
  return check({.speech = speech, .reply = reply, .wrote = false, .asked = true, .callsConfirmed = false});
}
}

TEST_CASE("every row names a slot the turn can ask about, with both languages and a non-empty cue list")
{
  std::vector<std::string_view> slots;
  for (const CueRow& row : cueRows()) {
    CHECK_FALSE(row.slot.empty());
    CHECK_FALSE(row.language.empty());
    CHECK_FALSE(row.cues.empty());
    if (std::ranges::find(slots, row.slot) == slots.end())
      slots.push_back(row.slot);
  }
  CHECK(slots.size() == 19);
  for (const std::string_view slot : slots) {
    CHECK_FALSE(cuesFor({.slot = slot, .lang = "es"}).empty());
    CHECK_FALSE(cuesFor({.slot = slot, .lang = "en"}).empty());
  }
}

TEST_CASE("a slot the turn never asks about has no row at all")
{
  for (const char* slot : {"limit", "status", "priority", "view", "all_day", "ends_at", "target_at", "project_id",
                           "include_done", "type", "confidence", "predicate", "value", "unknown_slot"})
    CHECK(cuesFor({.slot = slot, .lang = "es"}).empty());
}

TEST_CASE("a reply carrying a cue for the act's slot is accepted, and one carrying only another slot's cue is not")
{
  CHECK(asks("starts_at", "es", "¿Para qué día y a qué hora lo agendo?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "en", "When should I put it?") == GuardVerdict::Pass);
  CHECK(asks("title", "es", "¿Cómo se llama el evento?") == GuardVerdict::Pass);
  CHECK(asks("title", "en", "What is it called?") == GuardVerdict::Pass);
  CHECK(asks("name", "es", "¿Cómo se llama el proyecto nuevo?") == GuardVerdict::Pass);
  CHECK(asks("query", "en", "Which one should I look up?") == GuardVerdict::Pass);
  CHECK(asks("subject", "es", "¿De quién es el pasaporte?") == GuardVerdict::Pass);
  CHECK(asks("camera", "en", "Which camera do you mean?") == GuardVerdict::Pass);
  CHECK(asks("project", "es", "¿En cuál proyecto va?") == GuardVerdict::Pass);
  CHECK(asks("module", "en", "What module is that?") == GuardVerdict::Pass);
  CHECK(asks("location", "es", "¿Dónde es?") == GuardVerdict::Pass);
  CHECK(asks("environment", "en", "Which place is that?") == GuardVerdict::Pass);
  CHECK(asks("mode", "es", "¿Qué modo pongo?") == GuardVerdict::Pass);
  CHECK(asks("screen", "es", "¿A qué pantalla la abro?") == GuardVerdict::Pass);
  CHECK(asks("due_at", "es", "¿Para cuándo vence?") == GuardVerdict::Pass);
  CHECK(asks("from", "es", "¿Desde cuándo quieres verlo?") == GuardVerdict::Pass);
  CHECK(asks("to", "en", "Until when should I look?") == GuardVerdict::Pass);
  CHECK(asks("event_id", "es", "¿Qué evento cancelo?") == GuardVerdict::Pass);
  CHECK(asks("task_id", "en", "Which task do you mean?") == GuardVerdict::Pass);

  CHECK(asks("subject", "es", "¿Dónde lo guardo?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("title", "en", "Which camera do you mean?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("title", "es", "¿Qué hora es?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("title", "en", "What time is it?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("name", "en", "What time is it?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("query", "en", "What time is it?") == GuardVerdict::SlotNotAsked);
  CHECK(asks("module", "es", "¿Qué pantalla abro?") == GuardVerdict::SlotNotAsked);
}

TEST_CASE("the English title list carries the noun and both call phrasings, each accepting its own reply")
{
  CHECK(asks("title", "en", "What's the title?") == GuardVerdict::Pass);
  CHECK(asks("title", "en", "What's it called?") == GuardVerdict::Pass);
  CHECK(asks("title", "en", "What should I call it?") == GuardVerdict::Pass);
}

TEST_CASE("a cue for a slot the act already knows is a re-ask, whichever slot it names")
{
  Json::Value known(Json::objectValue);
  known["starts_at"] = "2026-10-08T17:00:00+00:00";
  known["title"] = "Cena con Marta";
  CHECK(asks("subject", "es", "¿De quién es? ¿Y a qué hora?", known) == GuardVerdict::SlotReasked);
  CHECK(asks("subject", "es", "¿De quién es?", known) == GuardVerdict::Pass);
  CHECK(asks("subject", "es", "¿De quién es? Dime el título del evento.", known) == GuardVerdict::SlotReasked);

  Json::Value projectKnown(Json::objectValue);
  projectKnown["project"] = "Casa";
  CHECK(asks("subject", "es", "¿De quién es? ¿En cuál proyecto?", projectKnown) == GuardVerdict::SlotReasked);
  CHECK(asks("subject", "en", "Whose is it? Which project?", projectKnown) == GuardVerdict::SlotReasked);
}

TEST_CASE("a slot with no cue row for one language falls back to the row the other language carries")
{
  CHECK_FALSE(cuesFor({.slot = "to", .lang = "en"}).empty());
  CHECK_FALSE(cuesFor({.slot = "to", .lang = "es"}).empty());
  CHECK(cuesFor({.slot = "task_id", .lang = "es"}).size() == cuesFor({.slot = "task_id", .lang = "en"}).size());
  CHECK(cuesFor({.slot = "task_id", .lang = "es"}).front() != cuesFor({.slot = "task_id", .lang = "en"}).front());
}

TEST_CASE("every action a confirmation or a choice can name has a label and a marker per language")
{
  const std::vector<std::string_view> tools{"calendar.create_event",
                                            "calendar.cancel_event",
                                            "task.create",
                                            "task.complete",
                                            "memory.remember",
                                            "memory.remind",
                                            "memory.forget",
                                            "app.show_camera",
                                            "app.set_guard_mode",
                                            "modules.enable"};
  for (const std::string_view tool : tools) {
    CHECK_FALSE(slotActionName({.tool = tool, .lang = "es"}).empty());
    CHECK_FALSE(slotActionName({.tool = tool, .lang = "en"}).empty());
    CHECK_FALSE(actionMarkers({.tool = tool, .lang = "es"}).empty());
    CHECK_FALSE(actionMarkers({.tool = tool, .lang = "en"}).empty());
  }
  CHECK(slotActionName({.tool = "tool.unknown", .lang = "es"}) == "que lo haga");
  CHECK(slotActionName({.tool = "tool.unknown", .lang = "en"}) == "do it");
  CHECK(actionMarkers({.tool = "tool.unknown", .lang = "es"}).empty());
}

TEST_CASE("a marker is a stem, so the model's own inflection still names the action")
{
  CHECK(chooseOf("es", "¿Quieres que lo agende o que lo olvide?") == GuardVerdict::Pass);
  CHECK(chooseOf("es", "¿Lo agendo o lo olvido?") == GuardVerdict::Pass);
  CHECK(chooseOf("en", "Should I schedule it or forget it?") == GuardVerdict::Pass);
  CHECK(chooseOf("es", "¿Lo agendo o lo guardo?") == GuardVerdict::OptionsIncomplete);
}

TEST_CASE("every time cue the slot family carries is accepted, one per cue")
{
  CHECK(asks("when", "es", "¿Cuándo lo hago?") == GuardVerdict::Pass);
  CHECK(asks("when", "es", "¿Qué día lo hago?") == GuardVerdict::Pass);
  CHECK(asks("when", "es", "¿A qué hora lo hago?") == GuardVerdict::Pass);
  CHECK(asks("when", "es", "¿Qué fecha te sirve?") == GuardVerdict::Pass);
  CHECK(asks("when", "es", "¿Qué hora te sirve?") == GuardVerdict::Pass);
  CHECK(asks("when", "en", "When should I do it?") == GuardVerdict::Pass);
  CHECK(asks("when", "en", "What day should I do it?") == GuardVerdict::Pass);
  CHECK(asks("when", "en", "What time should I do it?") == GuardVerdict::Pass);
  CHECK(asks("when", "en", "What date should I use?") == GuardVerdict::Pass);
  CHECK(asks("when", "en", "Does the time matter?") == GuardVerdict::Pass);

  CHECK(asks("starts_at", "es", "¿Qué fecha lo agendo?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "es", "¿A qué hora lo agendo?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "es", "¿Qué día y a qué hora?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "en", "What date should I schedule it?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "en", "What day and time?") == GuardVerdict::Pass);
}

TEST_CASE("the event and the memory text cues the briefing named are accepted, one per cue")
{
  CHECK(asks("event_id", "es", "¿Qué evento cancelo?") == GuardVerdict::Pass);
  CHECK(asks("event_id", "es", "¿Cuál evento cancelo?") == GuardVerdict::Pass);
  CHECK(asks("event_id", "en", "Which event should I cancel?") == GuardVerdict::Pass);
  CHECK(asks("text", "es", "¿Qué quieres que recuerde?") == GuardVerdict::Pass);
  CHECK(asks("text", "en", "What should I remember?") == GuardVerdict::Pass);
}

TEST_CASE("a slot's own display label is accepted as its cue, and another slot's is not")
{
  CHECK(asks("title", "es", "¿Cuál es el nombre de la tarea?") == GuardVerdict::Pass);
  CHECK(asks("title", "en", "What is the task name?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "es", "¿Me dices la fecha y hora?") == GuardVerdict::Pass);
  CHECK(asks("starts_at", "en", "Tell me the date and time.") == GuardVerdict::Pass);
  CHECK(asks("event_id", "es", "¿Cuál es el evento?") == GuardVerdict::Pass);
  CHECK(asks("title", "es", "¿Dónde lo guardo?") == GuardVerdict::SlotNotAsked);
}

TEST_CASE("every slot the lexicon covers has a human label of its own in both languages")
{
  std::vector<std::string_view> slots;
  for (const CueRow& row : cueRows())
    if (std::ranges::find(slots, row.slot) == slots.end())
      slots.push_back(row.slot);
  for (const std::string_view slot : slots) {
    const std::string_view es = slotLabel({.key = slot, .lang = "es"});
    const std::string_view en = slotLabel({.key = slot, .lang = "en"});
    CHECK_FALSE(es.empty());
    CHECK_FALSE(en.empty());
    CHECK(es != en);
    CHECK(es.find('.') == std::string_view::npos);
    CHECK(es.find('_') == std::string_view::npos);
    CHECK(en.find('.') == std::string_view::npos);
    CHECK(en.find('_') == std::string_view::npos);
  }
}

TEST_CASE("every option a choice can name has a display label of its own in both languages")
{
  const std::vector<std::string_view> tools{"calendar.create_event",
                                            "calendar.cancel_event",
                                            "calendar.list_events",
                                            "task.create",
                                            "task.complete",
                                            "memory.remember",
                                            "memory.remind",
                                            "memory.forget",
                                            "app.show_camera",
                                            "app.set_guard_mode"};
  for (const std::string_view tool : tools) {
    const std::string_view es = optionLabel({.key = tool, .lang = "es"});
    const std::string_view en = optionLabel({.key = tool, .lang = "en"});
    CHECK_FALSE(es.empty());
    CHECK_FALSE(en.empty());
    CHECK(es != en);
    CHECK(es.find('.') == std::string_view::npos);
    CHECK(es.find('_') == std::string_view::npos);
    CHECK(en.find('.') == std::string_view::npos);
    CHECK(en.find('_') == std::string_view::npos);
  }
  CHECK(optionLabel({.key = "tool.unknown", .lang = "es"}).empty());
}

TEST_CASE("a request cue exists per language and never a question mark")
{
  CHECK_FALSE(requestCues("es").empty());
  CHECK_FALSE(requestCues("en").empty());
  CHECK(requestCues("es").front() != requestCues("en").front());
}
